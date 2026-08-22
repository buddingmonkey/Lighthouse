#import "LighthouseVisionBridge.h"
#import <ARKit/ARKit.h>
#import <Metal/Metal.h>
#import <simd/simd.h>

static bool ClearPass(cp_drawable_t drawable, id<MTLCommandBuffer> commandBuffer, size_t textureIndex,
                      NSUInteger slice, NSUInteger arrayLength, MTLViewport viewport) {
    if (textureIndex >= cp_drawable_get_texture_count(drawable)) {
        return false;
    }

    id<MTLTexture> color = cp_drawable_get_color_texture(drawable, textureIndex);
    id<MTLTexture> depth = cp_drawable_get_depth_texture(drawable, textureIndex);
    if (slice >= color.arrayLength || slice >= depth.arrayLength) {
        return false;
    }

    NSUInteger count = MIN(arrayLength, MIN(color.arrayLength - slice, depth.arrayLength - slice));
    if (count == 0) {
        return false;
    }

    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = color;
    pass.colorAttachments[0].slice = slice;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
    pass.depthAttachment.texture = depth;
    pass.depthAttachment.slice = slice;
    pass.depthAttachment.loadAction = MTLLoadActionClear;
    pass.depthAttachment.storeAction = MTLStoreActionStore;
    pass.depthAttachment.clearDepth = 0;
    pass.renderTargetArrayLength = count;
    if (textureIndex < cp_drawable_get_rasterization_rate_map_count(drawable)) {
        pass.rasterizationRateMap = cp_drawable_get_rasterization_rate_map(drawable, textureIndex);
    }

    id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:pass];
    if (encoder == nil) {
        return false;
    }
    [encoder setViewport:viewport];
    [encoder endEncoding];
    return true;
}

static bool ClearDrawable(cp_drawable_t drawable, id<MTLCommandBuffer> commandBuffer,
                          cp_layer_renderer_layout layout) {
    size_t viewCount = cp_drawable_get_view_count(drawable);
    if (viewCount == 0) {
        return false;
    }

    if (layout == cp_layer_renderer_layout_layered) {
        cp_view_texture_map_t map = cp_view_get_view_texture_map(cp_drawable_get_view(drawable, 0));
        return ClearPass(drawable, commandBuffer, cp_view_texture_map_get_texture_index(map),
                         cp_view_texture_map_get_slice_index(map), viewCount, cp_view_texture_map_get_viewport(map));
    }
    if (layout == cp_layer_renderer_layout_shared) {
        cp_view_texture_map_t map = cp_view_get_view_texture_map(cp_drawable_get_view(drawable, 0));
        return ClearPass(drawable, commandBuffer, cp_view_texture_map_get_texture_index(map),
                         cp_view_texture_map_get_slice_index(map), 1, cp_view_texture_map_get_viewport(map));
    }

    for (size_t i = 0; i < viewCount; ++i) {
        cp_view_texture_map_t map = cp_view_get_view_texture_map(cp_drawable_get_view(drawable, i));
        if (!ClearPass(drawable, commandBuffer, cp_view_texture_map_get_texture_index(map),
                       cp_view_texture_map_get_slice_index(map), 1, cp_view_texture_map_get_viewport(map))) {
            return false;
        }
    }
    return true;
}

struct ScreenPipeline {
    id<MTLRenderPipelineState> render = nil;
    id<MTLDepthStencilState> depth = nil;
};

static bool InitScreenPipeline(ScreenPipeline& pipeline, id<MTLDevice> device, cp_drawable_t drawable) {
    id<MTLTexture> color = cp_drawable_get_color_texture(drawable, 0);
    id<MTLTexture> depth = cp_drawable_get_depth_texture(drawable, 0);
    if (color == nil || depth == nil) {
        return false;
    }

    NSString* source = @R"(
        #include <metal_stdlib>
        using namespace metal;
        struct VertexOut { float4 position [[position]]; float2 uv; };
        vertex VertexOut screenVertex(uint vertexID [[vertex_id]], ushort ampID [[amplification_id]],
                                      constant float4x4* mvp [[buffer(0)]]) {
            const float2 positions[] = { {-0.6, -0.3375}, {0.6, -0.3375}, {-0.6, 0.3375}, {0.6, 0.3375} };
            const float2 uvs[] = { {0, 1}, {1, 1}, {0, 0}, {1, 0} };
            VertexOut out;
            out.position = mvp[ampID] * float4(positions[vertexID], 0, 1);
            out.uv = uvs[vertexID];
            return out;
        }
        fragment half4 screenFragment(VertexOut in [[stage_in]]) {
            float2 grid = abs(fract(in.uv * 8.0) - 0.5);
            float line = 1.0 - smoothstep(0.43, 0.49, max(grid.x, grid.y));
            float3 base = mix(float3(0.03, 0.08, 0.16), float3(0.08, 0.28, 0.55), in.uv.y);
            return half4(half3(base + line * 0.2), 1.0);
        }
    )";
    NSError* error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
    if (library == nil) {
        return false;
    }
    MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.vertexFunction = [library newFunctionWithName:@"screenVertex"];
    descriptor.fragmentFunction = [library newFunctionWithName:@"screenFragment"];
    descriptor.colorAttachments[0].pixelFormat = color.pixelFormat;
    descriptor.depthAttachmentPixelFormat = depth.pixelFormat;
    descriptor.rasterSampleCount = color.sampleCount;
    descriptor.maxVertexAmplificationCount = cp_drawable_get_view_count(drawable);
    pipeline.render = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];

    MTLDepthStencilDescriptor* depthDescriptor = [MTLDepthStencilDescriptor new];
    depthDescriptor.depthCompareFunction = MTLCompareFunctionGreaterEqual;
    depthDescriptor.depthWriteEnabled = YES;
    pipeline.depth = [device newDepthStencilStateWithDescriptor:depthDescriptor];
    return pipeline.render != nil && pipeline.depth != nil;
}

static simd_float4x4 ScreenMVP(cp_drawable_t drawable, size_t viewIndex, simd_float4x4 originFromDevice,
                              simd_float4x4 originFromScreen) {
    cp_view_t view = cp_drawable_get_view(drawable, viewIndex);
    simd_float4x4 originFromView = simd_mul(originFromDevice, cp_view_get_transform(view));
    simd_float4x4 projection = cp_drawable_compute_projection(
        drawable, cp_axis_direction_convention_right_up_back, viewIndex);
    return simd_mul(projection, simd_mul(simd_inverse(originFromView), originFromScreen));
}

static void DrawScreen(cp_drawable_t drawable, id<MTLCommandBuffer> commandBuffer, ScreenPipeline& pipeline,
                       cp_layer_renderer_layout layout, simd_float4x4 originFromDevice,
                       simd_float4x4 originFromScreen) {
    NSUInteger viewCount = cp_drawable_get_view_count(drawable);
    if (viewCount == 0 || viewCount > 2) {
        return;
    }
    if (layout == cp_layer_renderer_layout_layered) {
        cp_view_texture_map_t map = cp_view_get_view_texture_map(cp_drawable_get_view(drawable, 0));
        size_t textureIndex = cp_view_texture_map_get_texture_index(map);
        id<MTLTexture> color = cp_drawable_get_color_texture(drawable, textureIndex);
        id<MTLTexture> depth = cp_drawable_get_depth_texture(drawable, textureIndex);

        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = color;
        pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.depthAttachment.texture = depth;
        pass.depthAttachment.loadAction = MTLLoadActionLoad;
        pass.depthAttachment.storeAction = MTLStoreActionStore;
        pass.renderTargetArrayLength = viewCount;
        if (textureIndex < cp_drawable_get_rasterization_rate_map_count(drawable)) {
            pass.rasterizationRateMap = cp_drawable_get_rasterization_rate_map(drawable, textureIndex);
        }

        simd_float4x4 mvp[2];
        MTLViewport viewports[2];
        MTLVertexAmplificationViewMapping mappings[2];
        for (NSUInteger i = 0; i < viewCount; ++i) {
            cp_view_texture_map_t viewMap = cp_view_get_view_texture_map(cp_drawable_get_view(drawable, i));
            mvp[i] = ScreenMVP(drawable, i, originFromDevice, originFromScreen);
            viewports[i] = cp_view_texture_map_get_viewport(viewMap);
            mappings[i] = { static_cast<uint32_t>(i), static_cast<uint32_t>(i) };
        }

        id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:pass];
        [encoder setRenderPipelineState:pipeline.render];
        [encoder setDepthStencilState:pipeline.depth];
        [encoder setViewports:viewports count:viewCount];
        [encoder setVertexAmplificationCount:viewCount viewMappings:mappings];
        [encoder setVertexBytes:mvp length:sizeof(simd_float4x4) * viewCount atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        [encoder endEncoding];
        return;
    }

    for (size_t i = 0; i < viewCount; ++i) {
        cp_view_t view = cp_drawable_get_view(drawable, i);
        cp_view_texture_map_t map = cp_view_get_view_texture_map(view);
        size_t textureIndex = cp_view_texture_map_get_texture_index(map);
        NSUInteger slice = cp_view_texture_map_get_slice_index(map);
        id<MTLTexture> color = cp_drawable_get_color_texture(drawable, textureIndex);
        id<MTLTexture> depth = cp_drawable_get_depth_texture(drawable, textureIndex);

        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = color;
        pass.colorAttachments[0].slice = slice;
        pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.depthAttachment.texture = depth;
        pass.depthAttachment.slice = slice;
        pass.depthAttachment.loadAction = MTLLoadActionLoad;
        pass.depthAttachment.storeAction = MTLStoreActionStore;
        if (textureIndex < cp_drawable_get_rasterization_rate_map_count(drawable)) {
            pass.rasterizationRateMap = cp_drawable_get_rasterization_rate_map(drawable, textureIndex);
        }

        simd_float4x4 mvp = ScreenMVP(drawable, i, originFromDevice, originFromScreen);

        id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:pass];
        [encoder setRenderPipelineState:pipeline.render];
        [encoder setDepthStencilState:pipeline.depth];
        [encoder setViewport:cp_view_texture_map_get_viewport(map)];
        [encoder setVertexBytes:&mvp length:sizeof(mvp) atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        [encoder endEncoding];
    }
}

void LighthouseVisionRun(cp_layer_renderer_t renderer) {
    @autoreleasepool {
        id<MTLCommandQueue> queue = [cp_layer_renderer_get_device(renderer) newCommandQueue];
        if (queue == nil || !ar_world_tracking_provider_is_supported()) {
            return;
        }

        ar_session_t session = ar_session_create();
        ar_world_tracking_configuration_t trackingConfiguration = ar_world_tracking_configuration_create();
        ar_world_tracking_provider_t trackingProvider = ar_world_tracking_provider_create(trackingConfiguration);
        ar_data_providers_t providers = ar_data_providers_create();
        ar_data_providers_add_data_provider(providers, trackingProvider);
        ar_session_run(session, providers);
        ar_device_anchor_t deviceAnchor = ar_device_anchor_create();
        cp_layer_renderer_layout layout = cp_layer_renderer_configuration_get_layout(
            cp_layer_renderer_get_configuration(renderer));
        ScreenPipeline screenPipeline;
        simd_float4x4 originFromScreen = matrix_identity_float4x4;
        bool screenPositioned = false;

        for (;;) {
            cp_layer_renderer_state state = cp_layer_renderer_get_state(renderer);
            if (state == cp_layer_renderer_state_paused) {
                cp_layer_renderer_wait_until_running(renderer);
                continue;
            }
            if (state != cp_layer_renderer_state_running) {
                ar_session_stop(session);
                return;
            }

            @autoreleasepool {
                cp_frame_t frame = cp_layer_renderer_query_next_frame(renderer);
                if (frame == nullptr) {
                    continue;
                }
                cp_frame_timing_t timing = cp_frame_predict_timing(frame);
                if (timing == nullptr) {
                    continue;
                }
                cp_frame_start_update(frame);
                cp_frame_end_update(frame);

                cp_time_wait_until(cp_frame_timing_get_optimal_input_time(timing));

                id<MTLCommandBuffer> commandBuffer = [queue commandBuffer];
                if (commandBuffer == nil) {
                    continue;
                }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
                cp_drawable_t drawable = cp_frame_query_drawable(frame);
#pragma clang diagnostic pop
                if (drawable == nullptr) {
                    continue;
                }
                cp_frame_start_submission(frame);
                cp_frame_timing_t drawableTiming = cp_drawable_get_frame_timing(drawable);
                CFTimeInterval presentationTime = cp_time_to_cf_time_interval(
                    cp_frame_timing_get_presentation_time(drawableTiming));
                if (ar_world_tracking_provider_query_device_anchor_at_timestamp(
                        trackingProvider, presentationTime, deviceAnchor) == ar_device_anchor_query_status_success) {
                    cp_drawable_set_device_anchor(drawable, deviceAnchor);
                    simd_float4x4 originFromDevice =
                        ar_device_anchor_get_origin_from_anchor_transform(deviceAnchor);
                    if (!screenPositioned) {
                        simd_float4x4 deviceFromScreen = matrix_identity_float4x4;
                        deviceFromScreen.columns[3].z = -1.3f;
                        originFromScreen = simd_mul(originFromDevice, deviceFromScreen);
                        screenPositioned = true;
                    }
                    cp_drawable_set_depth_range(drawable, simd_make_float2(100.0f, 0.1f));
                    if (screenPipeline.render == nil) {
                        InitScreenPipeline(screenPipeline, cp_layer_renderer_get_device(renderer), drawable);
                    }

                    ClearDrawable(drawable, commandBuffer, layout);
                    if (screenPipeline.render != nil) {
                        DrawScreen(drawable, commandBuffer, screenPipeline, layout, originFromDevice, originFromScreen);
                    }
                } else {
                    ClearDrawable(drawable, commandBuffer, layout);
                }
                cp_drawable_encode_present(drawable, commandBuffer);
                [commandBuffer commit];
                cp_frame_end_submission(frame);
            }
        }
    }
}

#import "LighthouseVisionBridge.h"
#import <ARKit/ARKit.h>
#import <GameController/GameController.h>
#import <Metal/Metal.h>
#import <simd/simd.h>

#include <fast/backends/gfx_visionos.h>
#include <fast/backends/gfx_xr_view.h>

#include <mutex>
#include <vector>

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

static const uint32_t kGameTextureWidth = 1280;
static const uint32_t kGameTextureHeight = 720;
// A window placed with the head bowed hangs low but stands up, and only a steep look tips it.
static const float kRiseFlat = 0.35f;
static const float kRiseMax = 1.22f;

struct ScreenPipeline {
    id<MTLRenderPipelineState> render = nil;
    id<MTLRenderPipelineState> solid = nil;
    id<MTLRenderPipelineState> tracking = nil;
    id<MTLDepthStencilState> depth = nil;
    id<MTLTexture> game[2] = { nil, nil };
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
        struct VertexOut { float4 position [[position]]; float2 uv; ushort eye [[flat]]; };
        vertex VertexOut screenVertex(uint vertexID [[vertex_id]], ushort ampID [[amplification_id]],
                                      constant float4x4* mvp [[buffer(0)]], constant float2& halfSize [[buffer(1)]],
                                      constant ushort& eyeBase [[buffer(2)]]) {
            const float2 positions[] = { {-halfSize.x, -halfSize.y}, {halfSize.x, -halfSize.y},
                                         {-halfSize.x, halfSize.y}, {halfSize.x, halfSize.y} };
            const float2 uvs[] = { {0, 1}, {1, 1}, {0, 0}, {1, 0} };
            VertexOut out;
            out.position = mvp[ampID] * float4(positions[vertexID], 0, 1);
            out.uv = uvs[vertexID];
            // Amplified, the two eyes come out of one pass and ampID names them. Dedicated, each
            // eye is its own pass and the base says which.
            out.eye = eyeBase + ampID;
            return out;
        }
        vertex float4 trackingVertex(uint vertexID [[vertex_id]], ushort ampID [[amplification_id]],
                                     constant float4x4* mvp [[buffer(0)]], constant float4& rect [[buffer(1)]]) {
            const float2 corners[] = { {rect.x, rect.y}, {rect.z, rect.y}, {rect.x, rect.w}, {rect.z, rect.w} };
            return mvp[ampID] * float4(corners[vertexID], 0, 1);
        }
        fragment uint trackingFragment(constant uint& value [[buffer(0)]]) {
            return value;
        }
        fragment half4 solidFragment(constant float4& color [[buffer(0)]]) {
            return half4(color);
        }
        fragment half4 screenFragment(VertexOut in [[stage_in]], texture2d<float> gameLeft [[texture(0)]],
                                      texture2d<float> gameRight [[texture(1)]]) {
            constexpr sampler gameSampler(filter::linear, address::clamp_to_edge);
            float3 encoded = in.eye == 0 ? gameLeft.sample(gameSampler, in.uv).rgb
                                         : gameRight.sample(gameSampler, in.uv).rgb;
            float3 linearColor = select(encoded / 12.92, pow((encoded + 0.055) / 1.055, 2.4), encoded > 0.04045);
            return half4(half3(linearColor), 1.0);
        }
    )";
    NSError* error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
    if (library == nil) {
        NSLog(@"Lighthouse: the visionOS screen library did not compile: %@", error);
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
    if (pipeline.render == nil) {
        NSLog(@"Lighthouse: the visionOS screen pipeline failed: %@", error);
    }

    if (cp_drawable_get_tracking_areas_texture_count(drawable) > 0) {
        id<MTLTexture> tracking = cp_drawable_get_tracking_areas_texture(drawable, 0);
        MTLRenderPipelineDescriptor* trackingDescriptor = [MTLRenderPipelineDescriptor new];
        trackingDescriptor.vertexFunction = [library newFunctionWithName:@"trackingVertex"];
        trackingDescriptor.fragmentFunction = [library newFunctionWithName:@"trackingFragment"];
        trackingDescriptor.colorAttachments[0].pixelFormat = tracking.pixelFormat;
        trackingDescriptor.maxVertexAmplificationCount = cp_drawable_get_view_count(drawable);
        pipeline.tracking = [device newRenderPipelineStateWithDescriptor:trackingDescriptor error:&error];
        if (pipeline.tracking == nil) {
            NSLog(@"Lighthouse: the visionOS tracking pipeline failed: %@", error);
        }
    }

    MTLRenderPipelineDescriptor* solidDescriptor = [MTLRenderPipelineDescriptor new];
    solidDescriptor.vertexFunction = [library newFunctionWithName:@"trackingVertex"];
    solidDescriptor.fragmentFunction = [library newFunctionWithName:@"solidFragment"];
    solidDescriptor.colorAttachments[0].pixelFormat = color.pixelFormat;
    solidDescriptor.colorAttachments[0].blendingEnabled = YES;
    solidDescriptor.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
    solidDescriptor.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    solidDescriptor.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorSourceAlpha;
    solidDescriptor.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    solidDescriptor.depthAttachmentPixelFormat = depth.pixelFormat;
    solidDescriptor.rasterSampleCount = color.sampleCount;
    solidDescriptor.maxVertexAmplificationCount = cp_drawable_get_view_count(drawable);
    pipeline.solid = [device newRenderPipelineStateWithDescriptor:solidDescriptor error:&error];
    if (pipeline.solid == nil) {
        NSLog(@"Lighthouse: the visionOS handle pipeline failed: %@", error);
    }

    for (int eye = 0; eye < 2; ++eye) {
        pipeline.game[eye] = (__bridge id<MTLTexture>)Fast::GetVisionOSGameTexture(eye);
        if (pipeline.game[eye] == nil) {
            NSLog(@"Lighthouse: the visionOS game texture for eye %d was not published", eye);
        }
    }

    MTLDepthStencilDescriptor* depthDescriptor = [MTLDepthStencilDescriptor new];
    depthDescriptor.depthCompareFunction = MTLCompareFunctionGreaterEqual;
    depthDescriptor.depthWriteEnabled = YES;
    pipeline.depth = [device newDepthStencilStateWithDescriptor:depthDescriptor];
    return pipeline.render != nil && pipeline.game[0] != nil && pipeline.game[1] != nil && pipeline.depth != nil;
}

// The window's own controls: a bar to move it and a corner to resize it, in a row under the
// picture so neither covers the game. The identifiers are above every ImGui id, which is 32 bits.
static const uint64_t kMoveAreaId = 1ull << 40;
static const uint64_t kResizeAreaId = (1ull << 40) + 1;
static const size_t kWindowHandleCount = 3;

struct WindowHandle {
    simd_float4 Rect;  ///< left, bottom, right, top, in meters in the screen's own plane.
    simd_float4 Outer; ///< The same handle with the dark edge that holds it against a light room.
    uint64_t Identifier;
};

static void WindowHandles(simd_float2 halfSize, WindowHandle handles[kWindowHandleCount]) {
    const float side = 0.10f * halfSize.y;
    const float gap = 0.5f * side;
    const float edge = 0.2f * side;
    const float top = -halfSize.y - gap;
    const float bottom = top - side;
    const float barHalfWidth = 0.25f * halfSize.x;
    const simd_float4 rects[kWindowHandleCount] = {
        simd_make_float4(-barHalfWidth, bottom, barHalfWidth, top),
        simd_make_float4(-barHalfWidth - gap - side, bottom, -barHalfWidth - gap, top),
        simd_make_float4(barHalfWidth + gap, bottom, barHalfWidth + gap + side, top),
    };
    const uint64_t identifiers[kWindowHandleCount] = { kMoveAreaId, kResizeAreaId, kResizeAreaId };
    for (size_t i = 0; i < kWindowHandleCount; ++i) {
        handles[i].Rect = rects[i];
        handles[i].Outer = rects[i] + simd_make_float4(-edge, -edge, edge, edge);
        handles[i].Identifier = identifiers[i];
    }
}

static void DrawHandles(id<MTLRenderCommandEncoder> encoder, ScreenPipeline& pipeline, simd_float2 halfSize) {
    if (pipeline.solid == nil) {
        return;
    }
    WindowHandle handles[kWindowHandleCount];
    WindowHandles(halfSize, handles);
    [encoder setRenderPipelineState:pipeline.solid];

    // A dark edge under a light face, because the room behind can be any color.
    const simd_float4 edgeColor = simd_make_float4(0.0f, 0.0f, 0.0f, 0.55f);
    [encoder setFragmentBytes:&edgeColor length:sizeof(edgeColor) atIndex:0];
    for (size_t i = 0; i < kWindowHandleCount; ++i) {
        [encoder setVertexBytes:&handles[i].Outer length:sizeof(simd_float4) atIndex:1];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    }

    const simd_float4 faceColor = simd_make_float4(1.0f, 1.0f, 1.0f, 0.9f);
    [encoder setFragmentBytes:&faceColor length:sizeof(faceColor) atIndex:0];
    for (size_t i = 0; i < kWindowHandleCount; ++i) {
        [encoder setVertexBytes:&handles[i].Rect length:sizeof(simd_float4) atIndex:1];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    }
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
                       simd_float4x4 originFromScreen, simd_float2 halfSize) {
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
        [encoder setVertexBytes:&halfSize length:sizeof(halfSize) atIndex:1];
        const uint16_t eyeBase = 0;
        [encoder setVertexBytes:&eyeBase length:sizeof(eyeBase) atIndex:2];
        [encoder setFragmentTexture:pipeline.game[0] atIndex:0];
        [encoder setFragmentTexture:pipeline.game[1] atIndex:1];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        DrawHandles(encoder, pipeline, halfSize);
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
        [encoder setVertexBytes:&halfSize length:sizeof(halfSize) atIndex:1];
        const uint16_t eyeBase = (uint16_t)i;
        [encoder setVertexBytes:&eyeBase length:sizeof(eyeBase) atIndex:2];
        [encoder setFragmentTexture:pipeline.game[0] atIndex:0];
        [encoder setFragmentTexture:pipeline.game[1] atIndex:1];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        DrawHandles(encoder, pipeline, halfSize);
        [encoder endEncoding];
    }
}

static void DrawTrackingAreas(cp_drawable_t drawable, id<MTLCommandBuffer> commandBuffer, ScreenPipeline& pipeline,
                              cp_layer_renderer_layout layout, simd_float4x4 originFromDevice,
                              simd_float4x4 originFromScreen, simd_float2 halfSize, cp_layer_renderer_t renderer) {
    const NSUInteger viewCount = cp_drawable_get_view_count(drawable);
    if (pipeline.tracking == nil || viewCount == 0 || viewCount > 2 ||
        cp_drawable_get_tracking_areas_texture_count(drawable) == 0) {
        return;
    }

    const cp_tracking_area_render_value maxValue =
        cp_layer_renderer_properties_get_tracking_areas_max_value(cp_layer_renderer_get_properties(renderer));
    const size_t rectCount = Fast::GetVisionOSTrackingRectCount();

    const bool layered = layout == cp_layer_renderer_layout_layered;
    const NSUInteger passCount = layered ? 1 : viewCount;
    for (NSUInteger pass = 0; pass < passCount; ++pass) {
        cp_view_texture_map_t map = cp_view_get_view_texture_map(cp_drawable_get_view(drawable, pass));
        const size_t textureIndex = cp_view_texture_map_get_texture_index(map);
        if (textureIndex >= cp_drawable_get_tracking_areas_texture_count(drawable)) {
            continue;
        }
        id<MTLTexture> tracking = cp_drawable_get_tracking_areas_texture(drawable, textureIndex);

        MTLRenderPassDescriptor* descriptor = [MTLRenderPassDescriptor renderPassDescriptor];
        descriptor.colorAttachments[0].texture = tracking;
        descriptor.colorAttachments[0].loadAction = MTLLoadActionClear;
        descriptor.colorAttachments[0].storeAction = MTLStoreActionStore;
        descriptor.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
        if (layered) {
            descriptor.renderTargetArrayLength = viewCount;
        } else {
            descriptor.colorAttachments[0].slice = cp_view_texture_map_get_slice_index(map);
        }
        if (textureIndex < cp_drawable_get_rasterization_rate_map_count(drawable)) {
            descriptor.rasterizationRateMap = cp_drawable_get_rasterization_rate_map(drawable, textureIndex);
        }

        id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:descriptor];
        if (encoder == nil) {
            continue;
        }
        [encoder setRenderPipelineState:pipeline.tracking];

        simd_float4x4 mvp[2];
        if (layered) {
            MTLViewport viewports[2];
            MTLVertexAmplificationViewMapping mappings[2];
            for (NSUInteger i = 0; i < viewCount; ++i) {
                cp_view_texture_map_t viewMap = cp_view_get_view_texture_map(cp_drawable_get_view(drawable, i));
                mvp[i] = ScreenMVP(drawable, i, originFromDevice, originFromScreen);
                viewports[i] = cp_view_texture_map_get_viewport(viewMap);
                mappings[i] = { static_cast<uint32_t>(i), static_cast<uint32_t>(i) };
            }
            [encoder setViewports:viewports count:viewCount];
            [encoder setVertexAmplificationCount:viewCount viewMappings:mappings];
        } else {
            mvp[0] = ScreenMVP(drawable, pass, originFromDevice, originFromScreen);
            [encoder setViewport:cp_view_texture_map_get_viewport(map)];
        }
        [encoder setVertexBytes:mvp length:sizeof(simd_float4x4) * (layered ? viewCount : 1) atIndex:0];

        uint32_t added = 0;
        WindowHandle handles[kWindowHandleCount];
        WindowHandles(halfSize, handles);
        for (size_t i = 0; i < kWindowHandleCount; ++i) {
            cp_tracking_area_t area = cp_drawable_add_tracking_area(drawable, handles[i].Identifier);
            if (area == nullptr) {
                continue;
            }
            cp_tracking_area_add_automatic_hover_effect(area);
            uint32_t value = cp_tracking_area_get_render_value(area);
            ++added;
            [encoder setVertexBytes:&handles[i].Outer length:sizeof(simd_float4) atIndex:1];
            [encoder setFragmentBytes:&value length:sizeof(value) atIndex:0];
            [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        }

        for (size_t i = 0; i < rectCount; ++i) {
            const Fast::VisionOSTrackingRect rect = Fast::GetVisionOSTrackingRect(i);

            // A zero identifier is a window. It takes no tracking area and only hides what is
            // behind it, which is what the clear value already means.
            uint32_t value = 0;
            if (rect.Identifier != 0) {
                if (added >= maxValue) {
                    static bool sReported = false;
                    if (!sReported) {
                        sReported = true;
                        NSLog(@"Lighthouse: more tracking areas than the %u the layer allows", (unsigned)maxValue);
                    }
                    continue;
                }
                cp_tracking_area_t area = cp_drawable_add_tracking_area(drawable, rect.Identifier);
                if (area == nullptr) {
                    continue;
                }
                cp_tracking_area_add_automatic_hover_effect(area);
                value = cp_tracking_area_get_render_value(area);
                ++added;
            }

            // The item is in game texture pixels. The screen quad spans the whole texture.
            const float left = (rect.MinX / kGameTextureWidth * 2.0f - 1.0f) * halfSize.x;
            const float right = (rect.MaxX / kGameTextureWidth * 2.0f - 1.0f) * halfSize.x;
            const float top = (1.0f - rect.MinY / kGameTextureHeight * 2.0f) * halfSize.y;
            const float bottom = (1.0f - rect.MaxY / kGameTextureHeight * 2.0f) * halfSize.y;
            const simd_float4 corners = simd_make_float4(left, bottom, right, top);

            [encoder setVertexBytes:&corners length:sizeof(corners) atIndex:1];
            [encoder setFragmentBytes:&value length:sizeof(value) atIndex:0];
            [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        }
        [encoder endEncoding];
    }
}

extern "C" int SDL_main(int argc, char* argv[]);
extern "C" void SDL_SetMainReady(void);
extern "C" void port_setAppOnScreen(int onScreen);

namespace {

struct CompositorState {
    cp_layer_renderer_t Renderer = nullptr;
    id<MTLCommandQueue> Queue = nil;
    ar_world_tracking_provider_t TrackingProvider = nullptr;
    ar_device_anchor_t DeviceAnchor = nullptr;
    cp_layer_renderer_layout Layout = cp_layer_renderer_layout_dedicated;
    ScreenPipeline Screen;
    bool ScreenFailed = false;
    simd_float4x4 OriginFromScreen = matrix_identity_float4x4;
    simd_float4x4 ScreenRotation = matrix_identity_float4x4;
    simd_float3 PlacementHead = { 0.0f, 0.0f, 0.0f };
    simd_float3 PlacementDir = { 0.0f, 0.0f, -1.0f };
    Fast::VisionOSWindow Window = { 0.6f, 0.3375f, 1.3f };
    bool ScreenPositioned = false;
    bool Running = true;

    cp_frame_t Frame = nullptr;
    cp_drawable_array_t Drawables = nullptr;
    size_t DrawableCount = 0;
    simd_float4x4 OriginFromDevice = matrix_identity_float4x4;
    bool DeviceAnchorValid = false;
};

CompositorState gState;

// A pinch on a handle is a drag, and the events arrive on the main thread. Keep the last ray and
// let the render thread, which owns the window, act on it.
struct DragState {
    std::mutex Mutex;
    int Kind = 0; ///< 0 none, 1 move, 2 resize.
    bool Begin = false;
    simd_float3 Origin = { 0.0f, 0.0f, 0.0f };
    simd_float3 Direction = { 0.0f, 0.0f, -1.0f };
};

DragState gDrag;

bool CompositorIsRunning() {
    return gState.Running;
}

} // namespace

void LighthouseVisionSpatialEvent(int phase, int hasRay, uint64_t trackingArea, float originX, float originY,
                                  float originZ, float directionX, float directionY, float directionZ) {
    static Fast::VisionOSPointer sPointer{};

    if (trackingArea == kMoveAreaId || trackingArea == kResizeAreaId) {
        {
            std::lock_guard<std::mutex> lock(gDrag.Mutex);
            if (phase == 0 && hasRay != 0) {
                if (gDrag.Kind == 0) {
                    gDrag.Kind = trackingArea == kMoveAreaId ? 1 : 2;
                    gDrag.Begin = true;
                }
                gDrag.Origin = simd_make_float3(originX, originY, originZ);
                gDrag.Direction = simd_make_float3(directionX, directionY, directionZ);
            } else {
                gDrag.Kind = 0;
                gDrag.Begin = false;
            }
        }

        // A handle is not an ImGui item, so nothing may be held down while one is dragged.
        sPointer.Pressed = false;
        Fast::PushVisionOSPointer(sPointer);
        return;
    }

    if (hasRay != 0 && gState.ScreenPositioned) {
        // The ray is in world coordinates. The screen is a quad at the origin of originFromScreen,
        // facing -z, so take the ray into that frame and cross the z plane.
        const simd_float4x4 screenFromOrigin = simd_inverse(gState.OriginFromScreen);
        const simd_float3 origin = simd_mul(screenFromOrigin, simd_make_float4(originX, originY, originZ, 1.0f)).xyz;
        const simd_float3 direction =
            simd_mul(screenFromOrigin, simd_make_float4(directionX, directionY, directionZ, 0.0f)).xyz;

        if (direction.z != 0.0f) {
            const float t = -origin.z / direction.z;
            if (t > 0.0f) {
                const simd_float3 hit = origin + t * direction;
                const float halfWidth = gState.Window.HalfWidth;
                const float halfHeight = gState.Window.HalfHeight;
                if (fabsf(hit.x) <= halfWidth && fabsf(hit.y) <= halfHeight) {
                    sPointer.X = (hit.x + halfWidth) / (2.0f * halfWidth) * kGameTextureWidth;
                    sPointer.Y = (halfHeight - hit.y) / (2.0f * halfHeight) * kGameTextureHeight;
                    sPointer.Valid = true;
                }
            }
        }
    }

    sPointer.Identifier = trackingArea;
    // Hold the last place the ray met the screen, so the button comes up over the same item.
    sPointer.Pressed = (phase == 0) && (sPointer.Valid || trackingArea != 0);
    Fast::PushVisionOSPointer(sPointer);
}

namespace {

// Debugging only. Gaze cannot be scripted, so LIGHTHOUSE_VISION_TAPS="x,y@seconds;x,y@seconds"
// presses the screen without it. Remove with the other visionOS input logs.
void RunScriptedTaps() {
    struct ScriptedTap {
        float X;
        float Y;
        double At;
    };
    static std::vector<ScriptedTap> sTaps;
    static size_t sNext = 0;
    static double sStart = 0.0;
    static bool sLoaded = false;

    if (!sLoaded) {
        sLoaded = true;
        sStart = CACurrentMediaTime();
        for (const char* p = getenv("LIGHTHOUSE_VISION_TAPS"); p != nullptr && *p != '\0';) {
            ScriptedTap tap{};
            int used = 0;
            if (sscanf(p, "%f,%f@%lf%n", &tap.X, &tap.Y, &tap.At, &used) != 3) {
                break;
            }
            sTaps.push_back(tap);
            p += used;
            if (*p != ';') {
                break;
            }
            ++p;
        }
        if (!sTaps.empty()) {
            NSLog(@"Lighthouse: %zu scripted taps", sTaps.size());
        }
    }

    if (sNext >= sTaps.size() || CACurrentMediaTime() - sStart < sTaps[sNext].At) {
        return;
    }

    Fast::VisionOSPointer down{};
    down.X = sTaps[sNext].X;
    down.Y = sTaps[sNext].Y;
    down.Valid = true;
    down.Pressed = true;
    // The mask keeps the last value written for a pixel, so the last rectangle that holds the point
    // is the tracking area the system would report.
    for (size_t i = 0; i < Fast::GetVisionOSTrackingRectCount(); ++i) {
        const Fast::VisionOSTrackingRect rect = Fast::GetVisionOSTrackingRect(i);
        if (down.X >= rect.MinX && down.X <= rect.MaxX && down.Y >= rect.MinY && down.Y <= rect.MaxY) {
            down.Identifier = rect.Identifier;
        }
    }
    Fast::PushVisionOSPointer(down);
    Fast::VisionOSPointer up = down;
    up.Pressed = false;
    Fast::PushVisionOSPointer(up);
    NSLog(@"Lighthouse: scripted tap %zu at %.0f,%.0f", sNext, down.X, down.Y);
    ++sNext;
}

// SDL's UIKit app delegate posts the lifecycle events on iOS, and it is not in this build. The
// layer state says the same thing: paused is off screen and invalidated is the end of the run.
// Read it from the event pump, not from the frame, because a paused app opens no frame.
void CompositorPollState() {
    static bool sOnScreen = true;

    const cp_layer_renderer_state state = cp_layer_renderer_get_state(gState.Renderer);
    if (state != cp_layer_renderer_state_running && state != cp_layer_renderer_state_paused) {
        if (gState.Running) {
            NSLog(@"Lighthouse: the visionOS layer stopped, state %d", (int)state);
        }
        gState.Running = false;
        return;
    }

    const bool onScreen = state == cp_layer_renderer_state_running;
    if (onScreen != sOnScreen) {
        sOnScreen = onScreen;
        port_setAppOnScreen(onScreen ? 1 : 0);
    }
}

// Compositor Services never says what the panel runs at, and nothing can ask it for a rate: the
// only lever is cp_layer_renderer_set_minimum_frame_repeat_count, which renders less often, not
// more. So the cadence is measured from the presentation times. A dropped frame only ever makes
// the gap longer, so the shortest gap in a window is the cadence, and a whole window has to pass
// before the answer changes. Following the shortest gap frame by frame instead would let rounding
// walk the answer up and down forever.
void NoteFrameCadence(CFTimeInterval presentationTime) {
    static const int kWindow = 120;
    static CFTimeInterval sLast = 0.0;
    static double sShortest = 0.0;
    static int sCount = 0;

    if (sLast > 0.0) {
        const double delta = presentationTime - sLast;
        if (delta > 0.002 && delta < 0.2) {
            if (sShortest <= 0.0 || delta < sShortest) {
                sShortest = delta;
            }
            if (++sCount >= kWindow) {
                Fast::SetVisionOSRefreshRate((uint32_t)llround(1.0 / sShortest));
                sShortest = 0.0;
                sCount = 0;
            }
        }
    }
    sLast = presentationTime;
}

float Clamp(float value, float low, float high) {
    return value < low ? low : (value > high ? high : value);
}

float Smoothstep(float low, float high, float value) {
    const float t = Clamp((value - low) / (high - low), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// The window faces the viewer along the direction it hangs in. It takes the yaw always and a part
// of the rise, and it takes no roll at all, so a head held at an angle does not leave the window
// askew in the room.
void FaceScreen() {
    const simd_float3 dir = gState.PlacementDir;
    const float across = sqrtf(dir.x * dir.x + dir.z * dir.z);
    const simd_float3 flat =
        across > 1e-4f ? simd_make_float3(dir.x / across, 0.0f, dir.z / across) : simd_make_float3(0.0f, 0.0f, -1.0f);
    const float rise = Clamp(atan2f(dir.y, across), -kRiseMax, kRiseMax);
    const float pitch = rise * Smoothstep(kRiseFlat, kRiseMax, fabsf(rise));

    const simd_float3 face = simd_make_float3(flat.x * cosf(pitch), sinf(pitch), flat.z * cosf(pitch));
    const simd_float3 zAxis = -face;
    const simd_float3 xAxis = simd_normalize(simd_cross(simd_make_float3(0.0f, 1.0f, 0.0f), zAxis));
    const simd_float3 yAxis = simd_cross(zAxis, xAxis);
    gState.ScreenRotation = matrix_identity_float4x4;
    gState.ScreenRotation.columns[0] = simd_make_float4(xAxis, 0.0f);
    gState.ScreenRotation.columns[1] = simd_make_float4(yAxis, 0.0f);
    gState.ScreenRotation.columns[2] = simd_make_float4(zAxis, 0.0f);
}

void PlaceScreen(simd_float4x4 originFromDevice) {
    const simd_float3 look = -originFromDevice.columns[2].xyz;
    const float across = sqrtf(look.x * look.x + look.z * look.z);
    const simd_float3 flat =
        across > 1e-4f ? simd_make_float3(look.x / across, 0.0f, look.z / across) : simd_make_float3(0.0f, 0.0f, -1.0f);
    const float rise = Clamp(atan2f(look.y, across), -kRiseMax, kRiseMax);

    gState.PlacementHead = originFromDevice.columns[3].xyz;
    gState.PlacementDir = simd_make_float3(flat.x * cosf(rise), sinf(rise), flat.z * cosf(rise));
    FaceScreen();
}

void UpdateScreenTransform() {
    gState.OriginFromScreen = gState.ScreenRotation;
    gState.OriginFromScreen.columns[3] =
        simd_make_float4(gState.PlacementHead + gState.PlacementDir * gState.Window.Range, 1.0f);
}

bool ScreenPlaneHit(simd_float3 origin, simd_float3 direction, simd_float2* hit) {
    const simd_float4x4 screenFromOrigin = simd_inverse(gState.OriginFromScreen);
    const simd_float3 o = simd_mul(screenFromOrigin, simd_make_float4(origin, 1.0f)).xyz;
    const simd_float3 d = simd_mul(screenFromOrigin, simd_make_float4(direction, 0.0f)).xyz;
    if (d.z == 0.0f) {
        return false;
    }
    const float t = -o.z / d.z;
    if (t <= 0.0f) {
        return false;
    }
    const simd_float3 p = o + t * d;
    *hit = simd_make_float2(p.x, p.y);
    return true;
}

simd_float3 RotateBetween(simd_float3 from, simd_float3 to, simd_float3 value) {
    const simd_float3 axis = simd_cross(from, to);
    const float sine = simd_length(axis);
    if (sine < 1e-6f) {
        return value;
    }
    const simd_float3 unit = axis / sine;
    const float angle = atan2f(sine, simd_dot(from, to));
    return value * cosf(angle) + simd_cross(unit, value) * sinf(angle) +
           unit * simd_dot(unit, value) * (1.0f - cosf(angle));
}

// The move carries the window with the direction the ray points, and the resize keeps the corner
// under the ray. Both write the numbers the menu holds, so a slider reads back what a hand left.
void ApplyDrag() {
    int kind = 0;
    bool begin = false;
    simd_float3 origin = { 0.0f, 0.0f, 0.0f };
    simd_float3 direction = { 0.0f, 0.0f, -1.0f };
    {
        std::lock_guard<std::mutex> lock(gDrag.Mutex);
        kind = gDrag.Kind;
        begin = gDrag.Begin;
        gDrag.Begin = false;
        origin = gDrag.Origin;
        direction = gDrag.Direction;
    }
    if (kind == 0) {
        return;
    }
    const float length = simd_length(direction);
    if (length < 1e-4f) {
        return;
    }
    const simd_float3 ray = direction / length;

    static simd_float3 sGrabRay = { 0.0f, 0.0f, -1.0f };
    static simd_float3 sGrabDir = { 0.0f, 0.0f, -1.0f };
    static float sGrabScale = 1.0f;
    static float sGrabReach = 0.0f;

    if (kind == 1) {
        if (begin) {
            // The head has moved since the window was placed, so the range the window keeps is the
            // one it has from where the viewer stands now.
            const simd_float3 head = gState.OriginFromDevice.columns[3].xyz;
            const simd_float3 reach = gState.OriginFromScreen.columns[3].xyz - head;
            const float radius = simd_length(reach);
            if (radius < 1e-3f) {
                return;
            }
            gState.PlacementHead = head;
            gState.PlacementDir = reach / radius;
            Fast::SetXrWindowDistance(radius);
            sGrabRay = ray;
            sGrabDir = gState.PlacementDir;
        }
        gState.PlacementDir = RotateBetween(sGrabRay, ray, sGrabDir);
        FaceScreen();
        return;
    }

    simd_float2 hit = { 0.0f, 0.0f };
    if (!ScreenPlaneHit(origin, ray, &hit)) {
        return;
    }
    const float reach = simd_length(hit);
    if (begin) {
        sGrabScale = Fast::GetXrWindowScale();
        sGrabReach = reach;
    }
    if (sGrabReach > 1e-3f && reach > 1e-3f) {
        Fast::SetXrWindowScale(sGrabScale * reach / sGrabReach);
    }
}

bool CompositorOpenFrame() {
    RunScriptedTaps();
    @autoreleasepool {
    if (cp_layer_renderer_get_state(gState.Renderer) != cp_layer_renderer_state_running) {
        return false;
    }

    gState.Frame = cp_layer_renderer_query_next_frame(gState.Renderer);
    cp_frame_timing_t timing = gState.Frame != nullptr ? cp_frame_predict_timing(gState.Frame) : nullptr;
    if (timing == nullptr) {
        gState.Frame = nullptr;
        return false;
    }

    cp_frame_start_update(gState.Frame);
    cp_frame_end_update(gState.Frame);
    cp_time_wait_until(cp_frame_timing_get_optimal_input_time(timing));

    gState.Drawables = cp_frame_query_drawables(gState.Frame);
    gState.DrawableCount = gState.Drawables != nullptr ? cp_drawable_array_get_count(gState.Drawables) : 0;
    if (gState.DrawableCount == 0) {
        gState.Frame = nullptr;
        return false;
    }
    cp_drawable_t drawable = cp_drawable_array_get_drawable(gState.Drawables, 0);

    cp_frame_start_submission(gState.Frame);
    cp_frame_timing_t drawableTiming = cp_drawable_get_frame_timing(drawable);
    CFTimeInterval presentationTime =
        cp_time_to_cf_time_interval(cp_frame_timing_get_presentation_time(drawableTiming));
    NoteFrameCadence(presentationTime);

    gState.DeviceAnchorValid = ar_world_tracking_provider_query_device_anchor_at_timestamp(
                                   gState.TrackingProvider, presentationTime, gState.DeviceAnchor) ==
                               ar_device_anchor_query_status_success;
    if (gState.DeviceAnchorValid) {
        gState.OriginFromDevice = ar_device_anchor_get_origin_from_anchor_transform(gState.DeviceAnchor);
        gState.Window = Fast::GetVisionOSWindow();
        const bool recenter = Fast::TakeVisionOSRecenter();
        if (!gState.ScreenPositioned || recenter) {
            PlaceScreen(gState.OriginFromDevice);
            gState.ScreenPositioned = true;
        }
        // The range is applied every frame, so the menu can pull the window in and push it out
        // without placing it again.
        UpdateScreenTransform();
        ApplyDrag();
        gState.Window = Fast::GetVisionOSWindow();
        UpdateScreenTransform();
        for (size_t i = 0; i < gState.DrawableCount; ++i) {
            cp_drawable_t each = cp_drawable_array_get_drawable(gState.Drawables, i);
            cp_drawable_set_device_anchor(each, gState.DeviceAnchor);
            cp_drawable_set_depth_range(each, simd_make_float2(100.0f, 0.1f));
        }
        if (gState.Screen.render == nil && !gState.ScreenFailed) {
            gState.ScreenFailed =
                !InitScreenPipeline(gState.Screen, cp_layer_renderer_get_device(gState.Renderer), drawable);
        }

        // The window camera model wants each eye in the screen's own axes. The shell is what knows
        // where the screen stands, so it does that part and reports meters.
        const simd_float4x4 screenFromOrigin = simd_inverse(gState.OriginFromScreen);
        const size_t viewCount = cp_drawable_get_view_count(drawable);
        Fast::SetVisionOSViewCount((uint32_t)viewCount);
        for (size_t i = 0; i < viewCount && i < 2; ++i) {
            const simd_float4x4 originFromView =
                simd_mul(gState.OriginFromDevice, cp_view_get_transform(cp_drawable_get_view(drawable, i)));
            const simd_float3 eye = simd_mul(screenFromOrigin, originFromView.columns[3]).xyz;
            Fast::SetVisionOSEye((int)i, eye.x, eye.y, eye.z);
        }
    }
    return true;
    }
}

void CompositorCloseFrame() {
    if (gState.DrawableCount == 0) {
        return;
    }
    @autoreleasepool {

    // Made after Fast3D commits, so the queue runs the game frame first and the screen samples it.
    id<MTLCommandBuffer> commandBuffer = [gState.Queue commandBuffer];
    if (commandBuffer != nil) {
        for (size_t i = 0; i < gState.DrawableCount; ++i) {
            cp_drawable_t each = cp_drawable_array_get_drawable(gState.Drawables, i);
            ClearDrawable(each, commandBuffer, gState.Layout);
            if (gState.DeviceAnchorValid && gState.Screen.render != nil) {
                const simd_float2 halfSize = simd_make_float2(gState.Window.HalfWidth, gState.Window.HalfHeight);
                DrawScreen(each, commandBuffer, gState.Screen, gState.Layout, gState.OriginFromDevice,
                           gState.OriginFromScreen, halfSize);
                DrawTrackingAreas(each, commandBuffer, gState.Screen, gState.Layout, gState.OriginFromDevice,
                                  gState.OriginFromScreen, halfSize, gState.Renderer);
            }
            cp_drawable_encode_present(each, commandBuffer);
        }
        [commandBuffer commit];
    }
    cp_frame_end_submission(gState.Frame);

    gState.Drawables = nullptr;
    gState.DrawableCount = 0;
    gState.Frame = nullptr;
    }
}

} // namespace

// SDL keeps its keyboard inside the UIKit video driver, which visionos.cmake turns off, so SDL
// never sees a paired keyboard. Read it from the Game Controller framework instead. SDL takes the
// key code as a scancode itself, because both are the HID usage.
static void AttachKeyboard(GCKeyboard* keyboard) {
    if (keyboard == nil || keyboard.keyboardInput == nil) {
        return;
    }
    // The default handler queue is the main queue. Give the keyboard one of its own, the way SDL
    // does, so a busy main thread cannot hold the keys up.
    dispatch_queue_t queue = dispatch_queue_create("com.andreweiche.lighthouse.keyboard", DISPATCH_QUEUE_SERIAL);
    dispatch_set_target_queue(queue, dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_HIGH, 0));
    keyboard.handlerQueue = queue;
    keyboard.keyboardInput.keyChangedHandler =
        ^(GCKeyboardInput* input, GCDeviceButtonInput* key, GCKeyCode keyCode, BOOL pressed) {
            Fast::PushVisionOSKey((int)keyCode, pressed != NO);
        };
}

static void StartKeyboard() {
    AttachKeyboard(GCKeyboard.coalescedKeyboard);
    [[NSNotificationCenter defaultCenter] addObserverForName:GCKeyboardDidConnectNotification
                                                      object:nil
                                                       queue:nil
                                                  usingBlock:^(NSNotification* note) {
                                                      AttachKeyboard(note.object);
                                                  }];
}

void LighthouseVisionRun(cp_layer_renderer_t renderer) {
    id<MTLDevice> device = cp_layer_renderer_get_device(renderer);
    id<MTLCommandQueue> queue = [device newCommandQueue];
    if (queue == nil || !ar_world_tracking_provider_is_supported()) {
        NSLog(@"Lighthouse: the visionOS compositor has no queue or no world tracking");
        return;
    }

    ar_session_t session = ar_session_create();
    ar_world_tracking_configuration_t trackingConfiguration = ar_world_tracking_configuration_create();
    ar_world_tracking_provider_t trackingProvider = ar_world_tracking_provider_create(trackingConfiguration);
    ar_data_providers_t providers = ar_data_providers_create();
    ar_data_providers_add_data_provider(providers, trackingProvider);
    ar_session_run(session, providers);

    gState.Renderer = renderer;
    gState.Queue = queue;
    gState.TrackingProvider = trackingProvider;
    gState.DeviceAnchor = ar_device_anchor_create();
    gState.Layout = cp_layer_renderer_configuration_get_layout(cp_layer_renderer_get_configuration(renderer));

    Fast::SetVisionOSCompositor((__bridge void*)device, (__bridge void*)queue, kGameTextureWidth, kGameTextureHeight);
    Fast::SetVisionOSFrameHooks(
        { CompositorOpenFrame, CompositorCloseFrame, CompositorIsRunning, CompositorPollState });

    // SDL_UIKitRunApp usually does this. Without it SDL_Init refuses every subsystem, and the
    // control deck gets no game controllers.
    SDL_SetMainReady();

    StartKeyboard();

    char program[] = "Lighthouse";
    char* argv[] = { program, nullptr };
    SDL_main(1, argv);

    ar_session_stop(session);

    // The scene is a compositor layer and nothing else. Once the layer is gone there is no way to
    // draw, so the app leaves rather than hold an empty process.
    exit(0);
}

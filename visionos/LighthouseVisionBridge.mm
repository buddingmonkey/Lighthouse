#import "LighthouseVisionBridge.h"
#import <ARKit/ARKit.h>
#import <GameController/GameController.h>
#import <Metal/Metal.h>
#import <simd/simd.h>

#include <fast/backends/gfx_visionos.h>

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
static const float kScreenHalfWidth = 0.6f;
static const float kScreenHalfHeight = 0.3375f;
static const float kScreenRange = 1.3f;

struct ScreenPipeline {
    id<MTLRenderPipelineState> render = nil;
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
                                      constant float4x4* mvp [[buffer(0)]], constant ushort& eyeBase [[buffer(2)]]) {
            const float2 positions[] = { {-0.6, -0.3375}, {0.6, -0.3375}, {-0.6, 0.3375}, {0.6, 0.3375} };
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
        const uint16_t eyeBase = 0;
        [encoder setVertexBytes:&eyeBase length:sizeof(eyeBase) atIndex:2];
        [encoder setFragmentTexture:pipeline.game[0] atIndex:0];
        [encoder setFragmentTexture:pipeline.game[1] atIndex:1];
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
        const uint16_t eyeBase = (uint16_t)i;
        [encoder setVertexBytes:&eyeBase length:sizeof(eyeBase) atIndex:2];
        [encoder setFragmentTexture:pipeline.game[0] atIndex:0];
        [encoder setFragmentTexture:pipeline.game[1] atIndex:1];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        [encoder endEncoding];
    }
}

static void DrawTrackingAreas(cp_drawable_t drawable, id<MTLCommandBuffer> commandBuffer, ScreenPipeline& pipeline,
                              cp_layer_renderer_layout layout, simd_float4x4 originFromDevice,
                              simd_float4x4 originFromScreen, cp_layer_renderer_t renderer) {
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
            const float left = (rect.MinX / kGameTextureWidth * 2.0f - 1.0f) * kScreenHalfWidth;
            const float right = (rect.MaxX / kGameTextureWidth * 2.0f - 1.0f) * kScreenHalfWidth;
            const float top = (1.0f - rect.MinY / kGameTextureHeight * 2.0f) * kScreenHalfHeight;
            const float bottom = (1.0f - rect.MaxY / kGameTextureHeight * 2.0f) * kScreenHalfHeight;
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
    bool ScreenPositioned = false;
    bool Running = true;

    cp_frame_t Frame = nullptr;
    cp_drawable_array_t Drawables = nullptr;
    size_t DrawableCount = 0;
    simd_float4x4 OriginFromDevice = matrix_identity_float4x4;
    bool DeviceAnchorValid = false;
};

CompositorState gState;

bool CompositorIsRunning() {
    return gState.Running;
}

} // namespace

void LighthouseVisionSpatialEvent(int phase, int hasRay, uint64_t trackingArea, float originX, float originY,
                                  float originZ, float directionX, float directionY, float directionZ) {
    static Fast::VisionOSPointer sPointer{};

    if (hasRay != 0 && gState.ScreenPositioned) {
        // The ray is in world coordinates. The screen is a quad at the origin of originFromScreen,
        // 1.2 by 0.675 meters, facing -z, so take the ray into that frame and cross the z plane.
        const simd_float4x4 screenFromOrigin = simd_inverse(gState.OriginFromScreen);
        const simd_float3 origin = simd_mul(screenFromOrigin, simd_make_float4(originX, originY, originZ, 1.0f)).xyz;
        const simd_float3 direction =
            simd_mul(screenFromOrigin, simd_make_float4(directionX, directionY, directionZ, 0.0f)).xyz;

        if (direction.z != 0.0f) {
            const float t = -origin.z / direction.z;
            if (t > 0.0f) {
                const simd_float3 hit = origin + t * direction;
                if (fabsf(hit.x) <= kScreenHalfWidth && fabsf(hit.y) <= kScreenHalfHeight) {
                    sPointer.X = (hit.x + kScreenHalfWidth) / (2.0f * kScreenHalfWidth) * kGameTextureWidth;
                    sPointer.Y = (kScreenHalfHeight - hit.y) / (2.0f * kScreenHalfHeight) * kGameTextureHeight;
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
// the gap longer, so the shortest gap is the cadence; it decays back out slowly, in case the
// compositor really does change rate.
void NoteFrameCadence(CFTimeInterval presentationTime) {
    static CFTimeInterval sLast = 0.0;
    static double sInterval = 0.0;

    if (sLast > 0.0) {
        const double delta = presentationTime - sLast;
        if (delta > 0.002 && delta < 0.2) {
            sInterval = (sInterval <= 0.0 || delta < sInterval) ? delta : sInterval * 1.0002;
        }
    }
    sLast = presentationTime;

    if (sInterval > 0.0) {
        Fast::SetVisionOSRefreshRate((uint32_t)llround(1.0 / sInterval));
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
        if (!gState.ScreenPositioned) {
            simd_float4x4 deviceFromScreen = matrix_identity_float4x4;
            deviceFromScreen.columns[3].z = -kScreenRange;
            gState.OriginFromScreen = simd_mul(gState.OriginFromDevice, deviceFromScreen);
            gState.ScreenPositioned = true;
        }
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
                DrawScreen(each, commandBuffer, gState.Screen, gState.Layout, gState.OriginFromDevice,
                           gState.OriginFromScreen);
                DrawTrackingAreas(each, commandBuffer, gState.Screen, gState.Layout, gState.OriginFromDevice,
                                  gState.OriginFromScreen, gState.Renderer);
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
    Fast::SetVisionOSScreen(kScreenHalfWidth, kScreenRange);
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

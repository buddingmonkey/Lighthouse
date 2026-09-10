#import "LighthouseVolumeBridge.h"

#import <ARKit/ARKit.h>
#import <Foundation/Foundation.h>
#import <GameController/GameController.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <simd/simd.h>

#include <fast/backends/gfx_visionos.h>
#include <fast/backends/gfx_xr_view.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <vector>

extern "C" int SDL_main(int argc, char* argv[]);
extern "C" void SDL_SetMainReady(void);
extern "C" void port_setAppOnScreen(int onScreen);
extern "C" void TouchControls_OpenMenu(void);
extern "C" void LighthouseVolumeEncodeCopy(void);

namespace {

const uint32_t kGameTextureWidth = 1280;
const uint32_t kGameTextureHeight = 720;
const float kRangeMin = 0.2f;
const float kRangeMax = 4.0f;
const float kRangeDefault = 1.3f;
const float kNominalIPD = 0.063f;
const int kHeadUnreported = -3;
const int kHeadNoTracking = -2;
const int kHeadNoQuad = -1;

struct Sample {
    simd_float3 Head = { 0.0f, 0.0f, kRangeDefault };
    simd_float4x4 ImmersiveFromQuad = matrix_identity_float4x4;
    float HalfWidth = 0.0f;
    float HalfHeight = 0.0f;
    int ScenePhase = 2;
    bool HeadValid = false;
    bool QuadValid = false;
};

struct VolumeState {
    id<MTLDevice> Device = nil;
    id<MTLCommandQueue> Queue = nil;
    ar_session_t Session = nullptr;
    ar_world_tracking_provider_t TrackingProvider = nullptr;
    ar_device_anchor_t DeviceAnchor = nullptr;
    dispatch_semaphore_t Frame = nullptr;
    std::mutex Mutex;
    Sample Latest;
    std::atomic<bool> Running{ true };
    bool Started = false;
    bool Stereo = false;
    bool Stopped = false;
    void (*ShutdownHandler)(void) = nullptr;

};

VolumeState gVolume;

void NoteCadence(double now) {
    constexpr int kWindow = 120;
    constexpr uint32_t kAgreeHz = 3;
    static double sLast = 0.0;
    static double sGaps[kWindow] = {};
    static int sCount = 0;
    static uint32_t sPrior = 0;

    if (sLast > 0.0) {
        const double delta = now - sLast;
        if (delta > 0.002 && delta < 0.2) {
            sGaps[sCount++] = delta;
            if (sCount >= kWindow) {
                std::sort(std::begin(sGaps), std::end(sGaps));
                const uint32_t hz = (uint32_t)llround(1.0 / sGaps[kWindow / 2]);
                const uint32_t moved = hz > sPrior ? hz - sPrior : sPrior - hz;
                if (sPrior != 0 && moved <= kAgreeHz) {
                    Fast::SetVisionOSRefreshRate(hz);
                }
                sPrior = hz;
                sCount = 0;
            }
        }
    }
    sLast = now;
}

float Clamp(float value, float low, float high) {
    return value < low ? low : (value > high ? high : value);
}

float LatchWindow(const Sample& sample) {
    static bool sLatched = false;
    static simd_float3 sQuad = { 0.0f, 0.0f, 0.0f };
    static float sHalfWidth = 0.0f;
    static float sRange = kRangeDefault;

    const simd_float3 quad = sample.ImmersiveFromQuad.columns[3].xyz;
    const bool moved = simd_distance(quad, sQuad) > 0.01f || fabsf(sample.HalfWidth - sHalfWidth) > 0.001f;
    if (!sLatched || moved) {
        sLatched = true;
        sQuad = quad;
        sHalfWidth = sample.HalfWidth;
        sRange = Clamp(sample.Head.z, kRangeMin, kRangeMax);
        Fast::SetVisionOSParallaxReference(sample.Head.x, sample.Head.y);
    }
    return sRange;
}

bool VolumeOpenFrame() {
    if (dispatch_semaphore_wait(gVolume.Frame, dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC)) != 0) {
        return false;
    }
    while (dispatch_semaphore_wait(gVolume.Frame, DISPATCH_TIME_NOW) == 0) {
    }

    Sample sample;
    {
        std::lock_guard<std::mutex> lock(gVolume.Mutex);
        sample = gVolume.Latest;
    }
    if (sample.HalfWidth <= 0.0f || sample.HalfHeight <= 0.0f) {
        return true;
    }

    const float range = sample.HeadValid ? LatchWindow(sample) : kRangeDefault;
    Fast::SetVisionOSWindow(sample.HalfWidth, sample.HalfHeight, range);

    Fast::SetVisionOSViewCount(gVolume.Stereo ? 2 : 1);
    const float half = 0.5f * kNominalIPD;
    const simd_float3 head = sample.HeadValid ? sample.Head : simd_make_float3(0.0f, 0.0f, range);
    Fast::SetVisionOSEye(0, head.x - half, head.y, head.z);
    Fast::SetVisionOSEye(1, head.x + half, head.y, head.z);
    return true;
}

void VolumeCloseFrame() {
    Fast::FlipVisionOSGameTextures();
    LighthouseVolumeEncodeCopy();
}

bool VolumeIsRunning() {
    return gVolume.Running.load(std::memory_order_acquire);
}

void VolumePollState() {
    static int sPhase = 2;

    int phase;
    {
        std::lock_guard<std::mutex> lock(gVolume.Mutex);
        phase = gVolume.Latest.ScenePhase;
    }
    if (phase == sPhase) {
        return;
    }
    sPhase = phase;
    port_setAppOnScreen(phase == 2 ? 1 : 0);
    char line[80];
    snprintf(line, sizeof(line), "the scene phase is %d, where 2 is active and 0 is background", phase);
    Fast::ReportVisionOS(line);
}

void AttachKeyboard(GCKeyboard* keyboard) {
    if (keyboard == nil || keyboard.keyboardInput == nil) {
        return;
    }
    static dispatch_queue_t queue;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        queue = dispatch_queue_create("com.harbormasters.lighthouse.keyboard", DISPATCH_QUEUE_SERIAL);
        dispatch_set_target_queue(queue, dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_HIGH, 0));
    });
    keyboard.handlerQueue = queue;
    keyboard.keyboardInput.keyChangedHandler =
        ^(GCKeyboardInput* input, GCDeviceButtonInput* key, GCKeyCode keyCode, BOOL pressed) {
            Fast::PushVisionOSKey((int)keyCode, pressed != NO);
        };
}

void StartKeyboard() {
    AttachKeyboard(GCKeyboard.coalescedKeyboard);
    [[NSNotificationCenter defaultCenter] addObserverForName:GCKeyboardDidConnectNotification
                                                      object:nil
                                                       queue:nil
                                                  usingBlock:^(NSNotification* note) {
                                                      AttachKeyboard(note.object);
                                                  }];
}

void StartTracking() {
    if (!ar_world_tracking_provider_is_supported()) {
        Fast::ReportVisionOS("world tracking is not supported");
        return;
    }
    ar_world_tracking_configuration_t configuration = ar_world_tracking_configuration_create();
    gVolume.TrackingProvider = ar_world_tracking_provider_create(configuration);
    ar_data_providers_t providers = ar_data_providers_create();
    ar_data_providers_add_data_provider(providers, gVolume.TrackingProvider);
    gVolume.Session = ar_session_create();
    ar_session_run(gVolume.Session, providers);
    gVolume.DeviceAnchor = ar_device_anchor_create();
}

} // namespace

void LighthouseVolumeStart(void* device, void* commandQueue, uint32_t width, uint32_t height) {
    if (gVolume.Started) {
        return;
    }
    gVolume.Started = true;
    gVolume.Device = (__bridge id<MTLDevice>)device;
    gVolume.Queue = (__bridge id<MTLCommandQueue>)commandQueue;
    gVolume.Frame = dispatch_semaphore_create(0);

    StartTracking();

    Fast::SetVisionOSRenderTarget(device, commandQueue, width, height);
    Fast::SetVisionOSFrameHooks({ VolumeOpenFrame, VolumeCloseFrame, VolumeIsRunning, VolumePollState });

    SDL_SetMainReady();
    StartKeyboard();

    NSThread* thread = [[NSThread alloc] initWithBlock:^{
        char program[] = "Lighthouse";
        char* argv[] = { program, nullptr };
        SDL_main(1, argv);

        LighthouseVolumeStop();
        void (*handler)(void) = gVolume.ShutdownHandler;
        if (handler != nullptr) {
            dispatch_async(dispatch_get_main_queue(), ^{
                handler();
            });
        } else {
            exit(0);
        }
    }];
    thread.name = @"Lighthouse Render Thread";
    thread.stackSize = 4 * 1024 * 1024;
    [thread start];
}

void LighthouseVolumeSetShutdownHandler(void (*handler)(void)) {
    gVolume.ShutdownHandler = handler;
}

void LighthouseVolumeStop(void) {
    if (gVolume.Stopped) {
        return;
    }
    gVolume.Stopped = true;
    gVolume.Running.store(false, std::memory_order_release);
    if (gVolume.Session != nullptr) {
        ar_session_stop(gVolume.Session);
    }
    if (gVolume.Frame != nullptr) {
        dispatch_semaphore_signal(gVolume.Frame);
    }
}

void LighthouseVolumeUpdate(LighthouseVolumeFrame frame) {
    if (!gVolume.Started) {
        return;
    }

    const double now = CACurrentMediaTime();
    NoteCadence(now);

    Sample sample;
    sample.ImmersiveFromQuad = frame.ImmersiveFromQuad;
    sample.HalfWidth = frame.HalfWidth;
    sample.HalfHeight = frame.HalfHeight;
    sample.ScenePhase = frame.ScenePhase;
    sample.QuadValid = frame.HasQuad;

    int headState;
    if (gVolume.TrackingProvider == nullptr) {
        headState = kHeadNoTracking;
    } else if (!frame.HasQuad) {
        headState = kHeadNoQuad;
    } else {
        const ar_device_anchor_query_status_t status = ar_world_tracking_provider_query_device_anchor_at_timestamp(
            gVolume.TrackingProvider, now, gVolume.DeviceAnchor);
        headState = (int)status;
        if (status == ar_device_anchor_query_status_success) {
            const simd_float4x4 originFromDevice =
                ar_device_anchor_get_origin_from_anchor_transform(gVolume.DeviceAnchor);
            const simd_float4 head = simd_mul(simd_inverse(frame.ImmersiveFromQuad), originFromDevice.columns[3]);
            sample.Head = head.xyz;
            sample.HeadValid = true;
        }
    }
    {
        static int sHeadState = kHeadUnreported;
        static double sHeadSaid = 0.0;
        const bool healthy = headState == (int)ar_device_anchor_query_status_success;
        if (headState != sHeadState || now - sHeadSaid > (healthy ? 60.0 : 10.0)) {
            sHeadState = headState;
            sHeadSaid = now;
            char line[96];
            snprintf(line, sizeof(line), "the head says %d, where %d is no world tracking and %d is no quad",
                     headState, kHeadNoTracking, kHeadNoQuad);
            Fast::ReportVisionOS(line);
        }
    }

    {
        std::lock_guard<std::mutex> lock(gVolume.Mutex);
        gVolume.Latest = sample;
    }
    dispatch_semaphore_signal(gVolume.Frame);
}

float LighthouseVolumeAspect(void) {
    return Fast::GetVisionOSPictureAspect();
}

void LighthouseVolumeNote(const char* text) {
    Fast::ReportVisionOS(text);
}

void LighthouseVolumeOpenMenu(void) {
    TouchControls_OpenMenu();
}

void LighthouseVolumePoint(float x, float y, bool pressed) {
    Fast::PushVisionOSPointer({ x, y, true, pressed });
}

size_t LighthouseVolumeHoverRects(LighthouseVolumeHoverRect* out, size_t max) {
    std::vector<Fast::VisionOSHoverRect> rects(max);
    const size_t count = Fast::CopyVisionOSHoverRects(rects.data(), max);
    for (size_t i = 0; i < count; ++i) {
        out[i] = { rects[i].MinX, rects[i].MinY, rects[i].MaxX, rects[i].MaxY, rects[i].Identifier };
    }
    return count;
}

void LighthouseVolumeNoteCopy(double) {
}

void LighthouseVolumeNotePrepare(double) {
}

void LighthouseVolumeNoteCopyGpu(double) {
}

void LighthouseVolumeSetStereo(bool stereo) {
    gVolume.Stereo = stereo;
}

void* LighthouseVolumeTexture(int eye) {
    return Fast::GetVisionOSReadyGameTexture(eye);
}

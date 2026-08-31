#import "LighthouseVolumeBridge.h"

#import <ARKit/ARKit.h>
#import <Foundation/Foundation.h>
#import <GameController/GameController.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <simd/simd.h>

#include <fast/backends/gfx_visionos.h>

#include <atomic>
#include <cmath>
#include <mutex>

extern "C" int SDL_main(int argc, char* argv[]);
extern "C" void SDL_SetMainReady(void);
extern "C" void port_setAppOnScreen(int onScreen);

namespace {

const uint32_t kGameTextureWidth = 1280;
const uint32_t kGameTextureHeight = 720;
const float kRangeMin = 0.2f;
const float kRangeMax = 4.0f;
const float kRangeDefault = 1.3f;

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
    std::atomic<bool> TextureReady{ false };
    std::atomic<bool> Running{ true };
    bool Started = false;
};

VolumeState gVolume;

// Nothing states the panel rate, so it is measured from the update times. A dropped frame only ever
// makes the gap longer, so the shortest gap in a window is the cadence.
void NoteCadence(double now) {
    static const int kWindow = 120;
    static double sLast = 0.0;
    static double sShortest = 0.0;
    static int sCount = 0;

    if (sLast > 0.0) {
        const double delta = now - sLast;
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
    sLast = now;
}

float Clamp(float value, float low, float high) {
    return value < low ? low : (value > high ? high : value);
}

// The range is the distance the window hangs at, not where the head is now. Head motion is only
// worth anything as a departure from a fixed reference, so a range that followed the head would
// take all the dolly parallax out of the picture.
float LatchedRange(const Sample& sample) {
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
    }
    return sRange;
}

bool VolumeOpenFrame() {
    // A volume that stops updating must not hold the game thread. It leaves without a frame and
    // comes back on the next update.
    if (dispatch_semaphore_wait(gVolume.Frame, dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC)) != 0) {
        return false;
    }

    Sample sample;
    {
        std::lock_guard<std::mutex> lock(gVolume.Mutex);
        sample = gVolume.Latest;
    }
    if (sample.HalfWidth <= 0.0f || sample.HalfHeight <= 0.0f) {
        return true;
    }

    const float range = sample.HeadValid ? LatchedRange(sample) : kRangeDefault;
    Fast::SetVisionOSWindow(sample.HalfWidth, sample.HalfHeight, range);

    // One picture for both eyes until the camera index switch lands. The backend takes the point
    // between the eyes when it has one view, so both report the head.
    Fast::SetVisionOSViewCount(1);
    if (sample.HeadValid) {
        Fast::SetVisionOSEye(0, sample.Head.x, sample.Head.y, sample.Head.z);
        Fast::SetVisionOSEye(1, sample.Head.x, sample.Head.y, sample.Head.z);
    }
    return true;
}

void VolumeCloseFrame() {
    Fast::FlipVisionOSGameTextures();
    gVolume.TextureReady.store(true, std::memory_order_release);
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
    if (phase == 0) {
        gVolume.Running.store(false, std::memory_order_release);
        return;
    }
    port_setAppOnScreen(phase == 2 ? 1 : 0);
}

// SDL keeps its keyboard inside the UIKit video driver, which visionos.cmake turns off, so SDL
// never sees a paired keyboard. Read it from the Game Controller framework instead.
void AttachKeyboard(GCKeyboard* keyboard) {
    if (keyboard == nil || keyboard.keyboardInput == nil) {
        return;
    }
    dispatch_queue_t queue = dispatch_queue_create("com.andreweiche.lighthouse.keyboard", DISPATCH_QUEUE_SERIAL);
    dispatch_set_target_queue(queue, dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_HIGH, 0));
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
        NSLog(@"Lighthouse volume: world tracking is not supported");
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

    Fast::SetVisionOSCompositor(device, commandQueue, width, height);
    Fast::SetVisionOSFrameHooks({ VolumeOpenFrame, VolumeCloseFrame, VolumeIsRunning, VolumePollState });

    // SDL_UIKitRunApp usually does this. Without it SDL_Init refuses every subsystem, and the
    // control deck gets no game controllers.
    SDL_SetMainReady();
    StartKeyboard();

    NSThread* thread = [[NSThread alloc] initWithBlock:^{
        char program[] = "Lighthouse";
        char* argv[] = { program, nullptr };
        SDL_main(1, argv);
        exit(0);
    }];
    thread.name = @"Lighthouse Render Thread";
    thread.stackSize = 4 * 1024 * 1024;
    [thread start];
}

void LighthouseVolumeUpdate(LighthouseVolumeFrame frame) {
    // The scene updates before the game is started, and there is nothing yet to tell.
    if (!gVolume.Started) {
        return;
    }

    const double now = CACurrentMediaTime();
    NoteCadence(now);

    // ARKit is not thread safe, so the query lives here and nowhere else.
    Sample sample;
    sample.ImmersiveFromQuad = frame.ImmersiveFromQuad;
    sample.HalfWidth = frame.HalfWidth;
    sample.HalfHeight = frame.HalfHeight;
    sample.ScenePhase = frame.ScenePhase;
    sample.QuadValid = frame.HasQuad;

    if (frame.HasQuad && gVolume.TrackingProvider != nullptr) {
        static bool sReported = false;
        static ar_device_anchor_query_status_t sStatus = ar_device_anchor_query_status_failure;
        const ar_device_anchor_query_status_t status = ar_world_tracking_provider_query_device_anchor_at_timestamp(
            gVolume.TrackingProvider, now, gVolume.DeviceAnchor);
        if (status != sStatus || !sReported) {
            // Nothing else states this. When the anchor stops answering the picture keeps drawing
            // and only the parallax dies, which is the kind of fault that is found late.
            sReported = true;
            sStatus = status;
            fprintf(stderr, "Lighthouse volume: the device anchor query says %d\n", (int)status);
            fflush(stderr);
        }
        if (status == ar_device_anchor_query_status_success) {
            const simd_float4x4 originFromDevice =
                ar_device_anchor_get_origin_from_anchor_transform(gVolume.DeviceAnchor);
            const simd_float4 head = simd_mul(simd_inverse(frame.ImmersiveFromQuad), originFromDevice.columns[3]);
            sample.Head = head.xyz;
            sample.HeadValid = true;
        }
    }

    {
        std::lock_guard<std::mutex> lock(gVolume.Mutex);
        gVolume.Latest = sample;
    }
    dispatch_semaphore_signal(gVolume.Frame);
}

void LighthouseVolumePoint(float x, float y, bool pressed) {
    Fast::PushVisionOSPointer({ x, y, 0, true, pressed });
}

void* LighthouseVolumeTakeTexture(int eye) {
    if (!gVolume.TextureReady.exchange(false, std::memory_order_acq_rel)) {
        return nullptr;
    }
    return Fast::GetVisionOSReadyGameTexture(eye);
}

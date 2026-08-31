#import "LighthouseVolumeBridge.h"

#import <ARKit/ARKit.h>
#import <Foundation/Foundation.h>
#import <GameController/GameController.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <simd/simd.h>

#include <fast/backends/gfx_visionos.h>

#include <spdlog/spdlog.h>

#include <atomic>
#include <cmath>
#include <mutex>

#include <SDL_joystick.h>

extern "C" int SDL_main(int argc, char* argv[]);
extern "C" void SDL_SetMainReady(void);
extern "C" void port_setAppOnScreen(int onScreen);

namespace {

const uint32_t kGameTextureWidth = 1280;
const uint32_t kGameTextureHeight = 720;
const float kRangeMin = 0.2f;
const float kRangeMax = 4.0f;
const float kRangeDefault = 1.3f;
// Apple keeps the wearer's own distance private, so the eyes are made from the head. The diorama
// gain compresses every disparity anyway, so a few millimeters of error is a few percent of depth.
const float kNominalIPD = 0.063f;

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
    bool Stereo = false;
    bool Stopped = false;
    void (*ShutdownHandler)(void) = nullptr;

    double FrameOpened = 0.0;
    double WaitTotal = 0.0;
    double DrawTotal = 0.0;
    double CopyTotal = 0.0;
    double NextRate = 0.0;
    int Updates = 0;
    int Frames = 0;
    std::atomic<int> PadEvents{ 0 };
    int PadPolls = 0;
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

// Head motion is only worth anything as a departure from a fixed reference. The reference is where
// the head stood when the window was placed, on all three axes, and it is taken again when the
// system moves the volume or changes its size.
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
    // A volume that stops updating must not hold the game thread. It leaves without a frame and
    // comes back on the next update.
    const double before = CACurrentMediaTime();
    if (dispatch_semaphore_wait(gVolume.Frame, dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC)) != 0) {
        return false;
    }
    // Take every slot that piled up while the last frame drew, so the game can be at most one frame
    // behind the volume. A counting semaphore left alone grows without bound whenever the game is
    // the slower of the two, and the game then never waits and never learns that it is behind.
    while (dispatch_semaphore_wait(gVolume.Frame, DISPATCH_TIME_NOW) == 0) {
    }
    gVolume.FrameOpened = CACurrentMediaTime();
    gVolume.WaitTotal += gVolume.FrameOpened - before;

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

    // The eyes are always reported. With one view the backend draws from the point between them,
    // which is the head again, so the same two numbers serve mono and stereo.
    Fast::SetVisionOSViewCount(gVolume.Stereo ? 2 : 1);
    // With no head there is still a picture to draw, and it is drawn for a head standing square in
    // front of the window at the range it hangs at. Stereo and the depth of the diorama both live;
    // only the parallax of leaning is lost. Reporting nothing would leave the eyes invalid and the
    // camera model would give up, which takes the stereo with it.
    const float half = 0.5f * kNominalIPD;
    const simd_float3 head = sample.HeadValid ? sample.Head : simd_make_float3(0.0f, 0.0f, range);
    Fast::SetVisionOSEye(0, head.x - half, head.y, head.z);
    Fast::SetVisionOSEye(1, head.x + half, head.y, head.z);
    return true;
}

void VolumeCloseFrame() {
    gVolume.DrawTotal += CACurrentMediaTime() - gVolume.FrameOpened;
    ++gVolume.Frames;
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
    // A volume that goes away parks the game the way an iOS app off screen does. The run ends only
    // when the game itself is finished, because a process that leaves takes the immersive space
    // down with it and that is the shell's job to do in order.
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

// The Game Controller framework refreshes a controller's values on its handler queue, and the
// default is the main queue. SDL polls those values, so a main thread busy with RealityKit at 90 Hz
// leaves SDL reading a stale pad. Give every controller a queue of its own, the way the keyboard
// already has one.
void AttachController(GCController* controller) {
    if (controller == nil) {
        return;
    }
    dispatch_queue_t queue = dispatch_queue_create("com.andreweiche.lighthouse.controller", DISPATCH_QUEUE_SERIAL);
    dispatch_set_target_queue(queue, dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_HIGH, 0));
    controller.handlerQueue = queue;
    controller.extendedGamepad.valueChangedHandler = ^(GCExtendedGamepad* pad, GCControllerElement* element) {
        gVolume.PadEvents.fetch_add(1, std::memory_order_relaxed);
    };
}

void StartControllers() {
    for (GCController* controller in GCController.controllers) {
        AttachController(controller);
    }
    [[NSNotificationCenter defaultCenter] addObserverForName:GCControllerDidConnectNotification
                                                      object:nil
                                                       queue:nil
                                                  usingBlock:^(NSNotification* note) {
                                                      AttachController(note.object);
                                                  }];
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
    StartControllers();

    NSThread* thread = [[NSThread alloc] initWithBlock:^{
        char program[] = "Lighthouse";
        char* argv[] = { program, nullptr };
        SDL_main(1, argv);

        // Leaving here would end the process with the immersive space still open, and visionOS
        // then refuses to open content for any app until the headset is restarted. The shell takes
        // the space down first.
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
    // The game thread may be waiting on a frame that will never come.
    if (gVolume.Frame != nullptr) {
        dispatch_semaphore_signal(gVolume.Frame);
    }
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

    // SDL reads the pad by polling the physical input profile, so poll the whole of it here and
    // count what moves. A count taken from one named profile cannot tell an unchanging pad from a
    // profile that is not there at all.
    {
        static float sSum = 0.0f;
        GCPhysicalInputProfile* profile = GCController.controllers.firstObject.physicalInputProfile;
        if (profile != nil) {
            float sum = 0.0f;
            for (NSString* key in profile.axes) {
                sum += fabsf(profile.axes[key].value);
            }
            for (NSString* key in profile.buttons) {
                sum += profile.buttons[key].value;
            }
            if (fabsf(sum - sSum) > 0.01f) {
                ++gVolume.PadPolls;
            }
            sSum = sum;
        }
    }

    // Nothing else states what the game is really presenting. The update rate is what the volume
    // offers, the frame rate is what the game takes, and the three times say which of them is the
    // one that costs.
    ++gVolume.Updates;
    if (now >= gVolume.NextRate) {
        if (gVolume.NextRate > 0.0 && gVolume.Frames > 0) {
            fprintf(stderr,
                    "Lighthouse volume: updates %d/s, frames %d/s, draw %.1f ms, wait %.1f ms, copy %.1f ms\n",
                    gVolume.Updates, gVolume.Frames, 1000.0 * gVolume.DrawTotal / gVolume.Frames,
                    1000.0 * gVolume.WaitTotal / gVolume.Frames, 1000.0 * gVolume.CopyTotal / gVolume.Frames);
            fflush(stderr);
            // Through spdlog, because only spdlog reaches the log file the device gives back.
            // The handler count and the polled count are two independent ways to ask the same
            // question, and SDL reads the pad the polled way.
            GCController* pad = GCController.controllers.firstObject;
            SPDLOG_INFO("xr input: phase {}, pad events {}/s, polled {}/s, gc pads {}, sdl joysticks {}, current {}, "
                        "extended {}, elements {}, name {}",
                        sample.ScenePhase, gVolume.PadEvents.exchange(0, std::memory_order_relaxed), gVolume.PadPolls,
                        (int)GCController.controllers.count, SDL_NumJoysticks(), GCController.current != nil ? 1 : 0,
                        pad.extendedGamepad != nil ? 1 : 0, (int)pad.physicalInputProfile.elements.count,
                        pad.productCategory != nil ? pad.productCategory.UTF8String : "none");
            gVolume.PadPolls = 0;
        }
        gVolume.NextRate = now + 1.0;
        gVolume.Updates = 0;
        gVolume.Frames = 0;
        gVolume.DrawTotal = 0.0;
        gVolume.WaitTotal = 0.0;
        gVolume.CopyTotal = 0.0;
    }
}

float LighthouseVolumeAspect(void) {
    return Fast::GetVisionOSPictureAspect();
}

void LighthouseVolumePoint(float x, float y, bool pressed) {
    Fast::PushVisionOSPointer({ x, y, 0, true, pressed });
}

void LighthouseVolumeNoteCopy(double seconds) {
    gVolume.CopyTotal += seconds;
}

void LighthouseVolumeSetStereo(bool stereo) {
    gVolume.Stereo = stereo;
}

bool LighthouseVolumeTakeFrame(void) {
    return gVolume.TextureReady.exchange(false, std::memory_order_acq_rel);
}

void* LighthouseVolumeTexture(int eye) {
    return Fast::GetVisionOSReadyGameTexture(eye);
}

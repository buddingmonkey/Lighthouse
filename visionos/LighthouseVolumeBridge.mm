#import "LighthouseVolumeBridge.h"

#import <ARKit/ARKit.h>
#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>
#import <simd/simd.h>

#include <cstdio>

namespace {

struct VolumeState {
    ar_session_t Session;
    ar_world_tracking_provider_t TrackingProvider;
    ar_device_anchor_t DeviceAnchor;
    bool Supported;
    double NextReport;
};

VolumeState gVolume = {};

NSString* Translation(simd_float4x4 matrix) {
    const simd_float3 p = matrix.columns[3].xyz;
    return [NSString stringWithFormat:@"%.3f %.3f %.3f", p.x, p.y, p.z];
}

} // namespace

void LighthouseVolumeStart(void) {
    if (gVolume.Session != nullptr) {
        return;
    }
    gVolume.Supported = ar_world_tracking_provider_is_supported();
    if (!gVolume.Supported) {
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

void LighthouseVolumeProbe(LighthouseVolumeSample sample) {
    const double now = CACurrentMediaTime();

    ar_device_anchor_query_status_t status = ar_device_anchor_query_status_failure;
    simd_float4x4 originFromDevice = matrix_identity_float4x4;
    if (gVolume.TrackingProvider != nullptr) {
        status = ar_world_tracking_provider_query_device_anchor_at_timestamp(gVolume.TrackingProvider, now,
                                                                            gVolume.DeviceAnchor);
        if (status == ar_device_anchor_query_status_success) {
            originFromDevice = ar_device_anchor_get_origin_from_anchor_transform(gVolume.DeviceAnchor);
        }
    }

    if (now < gVolume.NextReport) {
        return;
    }
    gVolume.NextReport = now + 1.0;

    NSString* arkit = @"none";
    if (sample.HasImmersiveSpace && status == ar_device_anchor_query_status_success) {
        const simd_float4x4 quadFromDevice = simd_mul(simd_inverse(sample.ImmersiveFromQuad), originFromDevice);
        arkit = Translation(quadFromDevice);
    }
    NSString* head = sample.HeadAnchored ? Translation(sample.QuadFromHead) : @"none";
    NSString* world = @"none";
    if (sample.WorldAnchored && status == ar_device_anchor_query_status_success) {
        world = Translation(simd_mul(sample.QuadFromWorld, originFromDevice));
    }

    // devicectl reaches the app's stderr and nothing else, so the report goes there and not to NSLog.
    fprintf(stderr,
            "Lighthouse volume: supported %d, phase %d, status %d, device %s, bounds center %.3f %.3f %.3f "
            "extents %.3f %.3f %.3f, head in quad: arkit %s, head anchor %s, world anchor %s\n",
            (int)gVolume.Supported, sample.ScenePhase, (int)status, Translation(originFromDevice).UTF8String,
            sample.BoundsCenter.x, sample.BoundsCenter.y, sample.BoundsCenter.z, sample.BoundsExtents.x,
            sample.BoundsExtents.y, sample.BoundsExtents.z, arkit.UTF8String, head.UTF8String, world.UTF8String);
    fflush(stderr);
}

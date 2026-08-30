import RealityKit
import Spatial
import SwiftUI
import UIKit

private final class VolumeProbe {
    var subscription: EventSubscription?
    var quad: Entity?
    var headAnchor: AnchorEntity?
    var worldAnchor: AnchorEntity?
    var bounds = BoundingBox()
    var phase: Int32 = 2
    let session = SpatialTrackingSession()
}

private struct LighthouseVolumeView: View {
    let probe: VolumeProbe
    @Environment(\.scenePhase) private var scenePhase

    var body: some View {
        GeometryReader3D { proxy in
            RealityView { content in
                let quad = ModelEntity(mesh: .generatePlane(width: 1.0, height: 0.5625),
                                       materials: [UnlitMaterial(color: .gray)])
                content.add(quad)
                probe.quad = quad

                let head = AnchorEntity(.head)
                content.add(head)
                probe.headAnchor = head

                let world = AnchorEntity(world: matrix_identity_float4x4)
                content.add(world)
                probe.worldAnchor = world

                probe.bounds = content.convert(proxy.frame(in: .local), from: .local, to: .scene)
                probe.subscription = content.subscribe(to: SceneEvents.Update.self) { _ in
                    report(probe)
                }
            } update: { content in
                probe.bounds = content.convert(proxy.frame(in: .local), from: .local, to: .scene)
            }
        }
        .task {
            let unavailable = await probe.session.run(.init(tracking: [.world]))
            print("Lighthouse volume: spatial tracking session unavailable \(String(describing: unavailable))")
            LighthouseVolumeStart()
        }
        .onChange(of: scenePhase, initial: true) { _, phase in
            switch phase {
            case .background: probe.phase = 0
            case .inactive: probe.phase = 1
            default: probe.phase = 2
            }
        }
    }
}

@MainActor
private func report(_ probe: VolumeProbe) {
    guard let quad = probe.quad else { return }
    var sample = LighthouseVolumeSample()
    sample.ScenePhase = probe.phase
    sample.BoundsCenter = probe.bounds.center
    sample.BoundsExtents = probe.bounds.extents
    if let immersiveFromQuad = quad.transformMatrix(relativeTo: .immersiveSpace) {
        sample.HasImmersiveSpace = true
        sample.ImmersiveFromQuad = immersiveFromQuad
    }
    if let head = probe.headAnchor, head.isAnchored {
        sample.HeadAnchored = true
        sample.QuadFromHead = head.transformMatrix(relativeTo: quad)
    }
    if let world = probe.worldAnchor, world.isAnchored {
        sample.WorldAnchored = true
        sample.QuadFromWorld = quad.transformMatrix(relativeTo: world).inverse
    }
    LighthouseVolumeProbe(sample)
}

@main
struct LighthouseVolumeApp: App {
    @State private var probe = VolumeProbe()

    var body: some SwiftUI.Scene {
        WindowGroup {
            LighthouseVolumeView(probe: probe)
        }
        .windowStyle(.volumetric)
        .defaultSize(width: 1.0, height: 0.5625, depth: 0.35, in: .meters)
        .volumeWorldAlignment(.gravityAligned)
    }
}

@preconcurrency import CompositorServices
import Foundation
import SwiftUI

private struct LighthouseLayerConfiguration: CompositorLayerConfiguration {
    func makeConfiguration(capabilities: LayerRenderer.Capabilities,
                           configuration: inout LayerRenderer.Configuration) {
        let foveationEnabled = capabilities.supportsFoveation
        configuration.isFoveationEnabled = foveationEnabled
        let options: LayerRenderer.Capabilities.SupportedLayoutsOptions = foveationEnabled ? [.foveationEnabled] : []
        let layouts = capabilities.supportedLayouts(options: options)
        configuration.layout = layouts.contains(.layered) ? .layered : .dedicated

        // Tracking areas let the system draw the gaze highlight itself, out of process, so the app
        // never learns where the eyes look.
        // The widest index the device offers, so the count of tracking areas is not the limit.
        // The default usage is already render target and shader read; narrowing it is refused.
        let formats = capabilities.supportedTrackingAreasFormats
        if let format = formats.first(where: { $0 == .r16Uint }) ?? formats.first {
            configuration.trackingAreasFormat = format
        }
    }
}

@main
struct LighthouseVisionApp: App {
    var body: some Scene {
        ImmersiveSpace {
            CompositorLayer(configuration: LighthouseLayerConfiguration()) { @MainActor renderer in
                renderer.onSpatialEvent = { events in
                    for event in events {
                        guard let ray = event.selectionRay else { continue }
                        let phase: Int32
                        switch event.phase {
                        case .active: phase = 0
                        case .ended: phase = 1
                        default: phase = 2
                        }
                        LighthouseVisionSpatialEvent(phase,
                                                     Float(ray.origin.x), Float(ray.origin.y), Float(ray.origin.z),
                                                     Float(ray.direction.x), Float(ray.direction.y),
                                                     Float(ray.direction.z))
                    }
                }
                let thread = Thread { LighthouseVisionRun(renderer) }
                thread.name = "Lighthouse Render Thread"
                thread.start()
            }
        }
        .immersionStyle(selection: .constant(.mixed), in: .mixed)
    }
}

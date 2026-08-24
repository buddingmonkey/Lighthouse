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
                        let phase: Int32
                        switch event.phase {
                        case .active: phase = 0
                        case .ended: phase = 1
                        default: phase = 2
                        }
                        // The end of a pinch can arrive with no ray. Report it anyway, or the
                        // button never comes up and nothing is ever clicked.
                        // The system says which tracking area it aimed at. That is the area it
                        // highlighted, so it always agrees with what the person saw.
                        let area = UInt64(event.trackingAreaIdentifier.rawValue)
                        if let ray = event.selectionRay {
                            LighthouseVisionSpatialEvent(phase, 1, area,
                                                         Float(ray.origin.x), Float(ray.origin.y),
                                                         Float(ray.origin.z), Float(ray.direction.x),
                                                         Float(ray.direction.y), Float(ray.direction.z))
                        } else {
                            LighthouseVisionSpatialEvent(phase, 0, area, 0, 0, 0, 0, 0, 0)
                        }
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

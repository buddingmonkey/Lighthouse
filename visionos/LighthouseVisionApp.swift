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

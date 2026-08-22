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
                let thread = Thread { LighthouseVisionRun(renderer) }
                thread.name = "Lighthouse Render Thread"
                thread.start()
            }
        }
        .immersionStyle(selection: .constant(.mixed), in: .mixed)
    }
}

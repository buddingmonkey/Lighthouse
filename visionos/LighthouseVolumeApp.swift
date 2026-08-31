import Foundation
import Metal
import RealityKit
import Spatial
import SwiftUI
import UIKit

private let kSpaceId = "LighthouseImmersiveSpace"
private let kTextureWidth = 1280
private let kTextureHeight = 720

private func note(_ text: String) {
    FileHandle.standardError.write("Lighthouse volume: \(text)\n".data(using: .utf8)!)
}

@MainActor
private final class VolumeState {
    let device: any MTLDevice
    let queue: any MTLCommandQueue
    let texture: LowLevelTexture
    let resource: TextureResource
    let session = SpatialTrackingSession()
    var quad: ModelEntity?
    var subscription: EventSubscription?
    var quadSize = SIMD2<Float>(0.0, 0.0)
    var phase: Int32 = 2

    init() {
        device = MTLCreateSystemDefaultDevice()!
        queue = device.makeCommandQueue()!
        // The game writes sRGB bytes into a plain BGRA8 target, so the picture is declared sRGB
        // here and RealityKit decodes it once when it samples.
        let descriptor = LowLevelTexture.Descriptor(pixelFormat: .bgra8Unorm_srgb,
                                                    width: kTextureWidth,
                                                    height: kTextureHeight,
                                                    textureUsage: [.shaderRead, .shaderWrite])
        texture = try! LowLevelTexture(descriptor: descriptor)
        resource = try! TextureResource(from: texture)
    }

    func makeQuad() -> ModelEntity {
        var material = UnlitMaterial()
        material.color = .init(tint: .white, texture: .init(resource))
        let entity = ModelEntity(mesh: .generatePlane(width: 1.0, height: 0.5625), materials: [material])
        quad = entity
        quadSize = SIMD2(1.0, 0.5625)
        return entity
    }

    // The picture keeps the game's shape and the volume keeps its own, so the quad takes the
    // largest 16 by 9 rectangle the volume holds.
    func size(to bounds: BoundingBox) {
        let aspect = Float(kTextureWidth) / Float(kTextureHeight)
        var width = bounds.extents.x
        var height = width / aspect
        if height > bounds.extents.y {
            height = bounds.extents.y
            width = height * aspect
        }
        if abs(width - quadSize.x) < 0.001 {
            return
        }
        quadSize = SIMD2(width, height)
        quad?.model?.mesh = .generatePlane(width: width, height: height)
    }

    func tick() {
        guard let quad else { return }
        var frame = LighthouseVolumeFrame()
        frame.ScenePhase = phase
        frame.HalfWidth = 0.5 * quadSize.x
        frame.HalfHeight = 0.5 * quadSize.y
        if let immersiveFromQuad = quad.transformMatrix(relativeTo: .immersiveSpace) {
            frame.HasQuad = true
            frame.ImmersiveFromQuad = immersiveFromQuad
        }
        LighthouseVolumeUpdate(frame)

        // Fast3D spreads framebuffer zero over several command buffers and commits it last, which
        // does not fit the one buffer replace(using:) wants. So the game keeps its own targets and
        // the finished one is copied here, on the same queue, after the game has committed.
        guard let raw = LighthouseVolumeTakeTexture(0),
              let source = Unmanaged<AnyObject>.fromOpaque(raw).takeUnretainedValue() as? any MTLTexture,
              let buffer = queue.makeCommandBuffer() else {
            return
        }
        let destination = texture.replace(using: buffer)
        if let blit = buffer.makeBlitCommandEncoder() {
            blit.copy(from: source,
                      sourceSlice: 0,
                      sourceLevel: 0,
                      sourceOrigin: MTLOrigin(x: 0, y: 0, z: 0),
                      sourceSize: MTLSize(width: kTextureWidth, height: kTextureHeight, depth: 1),
                      to: destination,
                      destinationSlice: 0,
                      destinationLevel: 0,
                      destinationOrigin: MTLOrigin(x: 0, y: 0, z: 0))
            blit.endEncoding()
        }
        buffer.commit()
    }
}

private struct LighthouseVolumeView: View {
    let state: VolumeState
    @Environment(\.scenePhase) private var scenePhase
    @Environment(\.openImmersiveSpace) private var openImmersiveSpace

    var body: some View {
        GeometryReader3D { proxy in
            RealityView { content in
                content.add(state.makeQuad())
                state.size(to: content.convert(proxy.frame(in: .local), from: .local, to: .scene))
                state.subscription = content.subscribe(to: SceneEvents.Update.self) { _ in
                    state.tick()
                }
            } update: { content in
                state.size(to: content.convert(proxy.frame(in: .local), from: .local, to: .scene))
            }
        }
        .task {
            _ = await state.session.run(.init(tracking: [.world]))

            // ARKit reports no head in the Shared Space. An empty mixed space beside the volume is
            // what makes the query answer; it draws nothing and hides nothing.
            let result = await openImmersiveSpace(id: kSpaceId)
            if case .opened = result {
                note("the immersive space is open")
            } else {
                note("the immersive space did not open, \(String(describing: result))")
            }

            LighthouseVolumeStart(Unmanaged.passUnretained(state.device as AnyObject).toOpaque(),
                                  Unmanaged.passUnretained(state.queue as AnyObject).toOpaque(),
                                  UInt32(kTextureWidth), UInt32(kTextureHeight))
        }
        .onChange(of: scenePhase, initial: true) { _, phase in
            switch phase {
            case .background: state.phase = 0
            case .inactive: state.phase = 1
            default: state.phase = 2
            }
        }
    }
}

@main
struct LighthouseVolumeApp: App {
    @State private var state = VolumeState()

    var body: some SwiftUI.Scene {
        WindowGroup {
            LighthouseVolumeView(state: state)
        }
        .windowStyle(.volumetric)
        .defaultSize(width: 1.0, height: 0.5625, depth: 0.35, in: .meters)
        .volumeWorldAlignment(.gravityAligned)

        ImmersiveSpace(id: kSpaceId) {
            RealityView { _ in }
        }
        .immersionStyle(selection: .constant(.mixed), in: .mixed)
    }
}

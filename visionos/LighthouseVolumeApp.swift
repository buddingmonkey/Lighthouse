import Foundation
import Metal
import QuartzCore
import RealityKit
import Spatial
import SwiftUI
import UIKit

private let kSpaceId = "LighthouseImmersiveSpace"
private let kEyeWidth = 1280
private let kTextureHeight = 720
// The two eyes stand side by side in one picture, and a camera index switch in the material gives
// each eye its own half. RealityKit has no other way to draw a thing differently for each eye.
private let kTextureWidth = 2 * kEyeWidth

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
    var bounds = BoundingBox()
    var aspect: Float = 16.0 / 9.0
    var phase: Int32 = 2

    private(set) var stereo = false
    private var eyeMaterial: (any RealityKit.Material)?

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
        let entity = ModelEntity(mesh: .generatePlane(width: 1.0, height: 0.5625), materials: [eyeMaterial ?? flat()])
        entity.components.set(InputTargetComponent())
        quad = entity
        quadSize = SIMD2(1.0, 0.5625)
        collide()
        return entity
    }

    func size(to bounds: BoundingBox) {
        self.bounds = bounds
        letterbox()
    }

    // The picture keeps the shape the game's own projection gives it, and the volume keeps the
    // shape the system gives it, so the quad takes the largest picture the volume holds.
    private func letterbox() {
        var width = bounds.extents.x
        var height = width / aspect
        if height > bounds.extents.y {
            height = bounds.extents.y
            width = height * aspect
        }
        if abs(width - quadSize.x) < 0.001 && abs(height - quadSize.y) < 0.001 {
            return
        }
        quadSize = SIMD2(width, height)
        quad?.model?.mesh = .generatePlane(width: width, height: height)
        collide()
    }

    private func flat() -> any RealityKit.Material {
        var material = UnlitMaterial()
        material.color = .init(tint: .white, texture: .init(resource))
        return material
    }

    // The camera index switch lives in a material graph, and only a file can hold one. Without it
    // there is no per eye path at all on visionOS, so one picture for both eyes is the fallback.
    func loadEyeMaterial() async {
        guard let url = Bundle.main.url(forResource: "GameScreen", withExtension: "usda") else {
            note("GameScreen.usda is not in the bundle")
            return
        }
        do {
            var material = try await ShaderGraphMaterial(named: "/Root/GameScreen", from: url)
            try material.setParameter(name: "GameTexture", value: .textureResource(resource))
            eyeMaterial = material
            stereo = true
            LighthouseVolumeSetStereo(true)
            quad?.model?.materials = [material]
            note("an eye each")
        } catch {
            note("the eye material did not load, \(error)")
        }
    }

    // A drag needs something to hit. The box is as thin as the picture it stands for.
    private func collide() {
        let shape = ShapeResource.generateBox(width: quadSize.x, height: quadSize.y, depth: 0.01)
        quad?.components.set(CollisionComponent(shapes: [shape], isStatic: true))
    }

    func point(_ value: EntityTargetValue<DragGesture.Value>, pressed: Bool) {
        guard let quad, quadSize.x > 0.0, quadSize.y > 0.0 else { return }
        let local = value.convert(value.location3D, from: .local, to: quad)
        let u = min(max(local.x / quadSize.x + 0.5, 0.0), 1.0)
        let v = min(max(0.5 - local.y / quadSize.y, 0.0), 1.0)
        LighthouseVolumePoint(u * Float(kEyeWidth), v * Float(kTextureHeight), pressed)
    }

    func tick() {
        guard let quad else { return }
        let shape = LighthouseVolumeAspect()
        if shape > 0.0, abs(shape - aspect) > 0.001 {
            aspect = shape
            letterbox()
        }
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
        guard LighthouseVolumeTakeFrame(), let buffer = queue.makeCommandBuffer() else { return }
        let started = CACurrentMediaTime()
        let destination = texture.replace(using: buffer)
        if let blit = buffer.makeBlitCommandEncoder() {
            for eye in 0..<(stereo ? 2 : 1) {
                guard let raw = LighthouseVolumeTexture(Int32(eye)),
                      let source = Unmanaged<AnyObject>.fromOpaque(raw).takeUnretainedValue() as? any MTLTexture
                else {
                    continue
                }
                blit.copy(from: source,
                          sourceSlice: 0,
                          sourceLevel: 0,
                          sourceOrigin: MTLOrigin(x: 0, y: 0, z: 0),
                          sourceSize: MTLSize(width: kEyeWidth, height: kTextureHeight, depth: 1),
                          to: destination,
                          destinationSlice: 0,
                          destinationLevel: 0,
                          destinationOrigin: MTLOrigin(x: eye * kEyeWidth, y: 0, z: 0))
            }
            blit.endEncoding()
        }
        buffer.commit()
        LighthouseVolumeNoteCopy(CACurrentMediaTime() - started)
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
            .gesture(
                DragGesture(minimumDistance: 0.0)
                    .targetedToAnyEntity()
                    .onChanged { state.point($0, pressed: true) }
                    .onEnded { state.point($0, pressed: false) }
            )
        }
        .task {
            await state.loadEyeMaterial()
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
                                  UInt32(kEyeWidth), UInt32(kTextureHeight))
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

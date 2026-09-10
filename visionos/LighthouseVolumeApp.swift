import Foundation
import GameController
import Metal
import QuartzCore
import RealityKit
import Spatial
import SwiftUI
import UIKit

private let kSpaceId = "LighthouseImmersiveSpace"
private let kHoverRectMax = 256
private let kEyeWidth = 1280
private let kTextureHeight = 720
private let kTextureWidth = 2 * kEyeWidth
private let kPictureAspect = Double(kEyeWidth) / Double(kTextureHeight)
private let kVolumeWidth = 1.0
private let kVolumeHeight = kVolumeWidth / kPictureAspect
private let kVolumeDepth = 0.35
private let kMenuGap = 14.0
private let kHoverLift = Float(0.002)
private let kHoverStep = Float(0.0001)

@MainActor private var gOpenSpace: OpenImmersiveSpaceAction?
@MainActor private var gDismissSpace: DismissImmersiveSpaceAction?
@MainActor private var gSpaceOpen = false
@MainActor private var gSpaceBusy = false
@MainActor private var gLeaving = false

@MainActor private func holdSpace(_ wanted: Bool) async {
    if gLeaving || gSpaceBusy || wanted == gSpaceOpen || gOpenSpace == nil {
        return
    }
    gSpaceBusy = true
    if wanted {
        if case .opened = await gOpenSpace?(id: kSpaceId) {
            gSpaceOpen = true
            note("the immersive space is open")
        } else {
            note("the immersive space did not open")
        }
    } else {
        await gDismissSpace?()
        gSpaceOpen = false
        note("the immersive space is given back")
    }
    gSpaceBusy = false
}

@MainActor private func leave() {
    if gLeaving {
        return
    }
    LighthouseVolumeStop()
    guard gSpaceOpen, let dismiss = gDismissSpace else {
        gLeaving = true
        exit(0)
    }
    gLeaving = true
    Task {
        await dismiss()
        exit(0)
    }
}

private func note(_ text: String) {
    LighthouseVolumeNote(text)
}

private final class CopyHandoff: @unchecked Sendable {
    private let lock = NSLock()
    private var pair: (any MTLCommandBuffer, any MTLTexture)?
    private var busy = false
    private var stereo = false
    private var done = false

    func setStereo() {
        lock.lock()
        stereo = true
        lock.unlock()
    }

    var eyes: Int {
        lock.lock()
        defer { lock.unlock() }
        return stereo ? 2 : 1
    }

    var hasRoom: Bool {
        lock.lock()
        defer { lock.unlock() }
        return pair == nil && !busy
    }

    func stash(_ buffer: any MTLCommandBuffer, _ destination: any MTLTexture) {
        lock.lock()
        pair = (buffer, destination)
        lock.unlock()
    }

    func take() -> (any MTLCommandBuffer, any MTLTexture)? {
        lock.lock()
        defer { lock.unlock() }
        guard let taken = pair else { return nil }
        pair = nil
        busy = true
        return taken
    }

    func finish() {
        lock.lock()
        busy = false
        done = true
        lock.unlock()
    }

    var copied: Bool {
        lock.lock()
        defer { lock.unlock() }
        return done
    }
}

private let gCopyHandoff = CopyHandoff()

@_cdecl("LighthouseVolumeEncodeCopy")
func lighthouseVolumeEncodeCopy() {
    guard let (buffer, destination) = gCopyHandoff.take() else { return }
    let started = CACurrentMediaTime()
    if let blit = buffer.makeBlitCommandEncoder() {
        for eye in 0..<gCopyHandoff.eyes {
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
    buffer.addCompletedHandler { done in
        LighthouseVolumeNoteCopyGpu(done.gpuEndTime - done.gpuStartTime)
    }
    buffer.commit()
    gCopyHandoff.finish()
    LighthouseVolumeNoteCopy(CACurrentMediaTime() - started)
}

private struct HoverRect: Identifiable, Equatable {
    let id: Int
    let frame: CGRect
    let item: Bool
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
    var aspect = Float(kPictureAspect)
    var phase: Int32 = 2
    private var quadLostAt = 0.0
    private var lastQuadTransform: float4x4?
    private var rawHover = [LighthouseVolumeHoverRect](repeating: LighthouseVolumeHoverRect(), count: kHoverRectMax)
    private var hoverEntities: [ModelEntity] = []
    private var hoverShown: [HoverRect] = []
    private var hoverQuad = SIMD2<Float>(0.0, 0.0)
    private var hoverMaterial: ShaderGraphMaterial?
    private var hoverNote: String?

    private var eyeMaterial: (any RealityKit.Material)?

    init() {
        device = MTLCreateSystemDefaultDevice()!
        queue = device.makeCommandQueue()!
        let descriptor = LowLevelTexture.Descriptor(pixelFormat: .bgra8Unorm_srgb,
                                                    width: kTextureWidth,
                                                    height: kTextureHeight,
                                                    textureUsage: [.shaderRead, .shaderWrite])
        texture = try! LowLevelTexture(descriptor: descriptor)
        resource = try! TextureResource(from: texture)
    }

    func makeQuad() -> ModelEntity {
        let size = SIMD2(Float(kVolumeWidth), Float(kVolumeHeight))
        let entity = ModelEntity(mesh: .generatePlane(width: size.x, height: size.y), materials: [eyeMaterial ?? flat()])
        entity.components.set(InputTargetComponent())
        quad = entity
        quadSize = size
        collide()
        return entity
    }

    func size(to bounds: BoundingBox) {
        self.bounds = bounds
        letterbox()
    }

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

    func loadEyeMaterial() async {
        guard let url = Bundle.main.url(forResource: "GameScreen", withExtension: "usda") else {
            note("GameScreen.usda is not in the bundle")
            return
        }
        do {
            var material = try await ShaderGraphMaterial(named: "/Root/GameScreen", from: url)
            try material.setParameter(name: "GameTexture", value: .textureResource(resource))
            eyeMaterial = material
            gCopyHandoff.setStereo()
            LighthouseVolumeSetStereo(true)
            quad?.model?.materials = [material]
            note("an eye each")
        } catch {
            note("the eye material did not load, \(error)")
        }
        do {
            var material = try await ShaderGraphMaterial(named: "/Root/HoverPlate", from: url)
            try material.setParameter(name: "GameTexture", value: .textureResource(resource))
            hoverMaterial = material
            hoverNote = "the hover material is ready"
        } catch {
            hoverNote = "the hover material did not load, \(error)"
        }
    }

    private func collide() {
        let shape = ShapeResource.generateBox(width: quadSize.x, height: quadSize.y, depth: 0.002)
        quad?.components.set(CollisionComponent(shapes: [shape], isStatic: true))
    }

    private func readHover() {
        guard hoverMaterial != nil else { return }
        let count = rawHover.withUnsafeMutableBufferPointer { buffer in
            LighthouseVolumeHoverRects(buffer.baseAddress, kHoverRectMax)
        }
        var next: [HoverRect] = []
        next.reserveCapacity(count)
        for index in 0..<count {
            let rect = rawHover[index]
            next.append(HoverRect(id: index,
                                  frame: CGRect(x: CGFloat(rect.MinX),
                                                y: CGFloat(rect.MinY),
                                                width: CGFloat(rect.MaxX - rect.MinX),
                                                height: CGFloat(rect.MaxY - rect.MinY)),
                                  item: rect.Identifier != 0))
        }
        if next != hoverShown || quadSize != hoverQuad {
            hoverShown = next
            hoverQuad = quadSize
            layOutHover(next)
        }
    }

    private func layOutHover(_ rects: [HoverRect]) {
        guard let quad, let material = hoverMaterial, quadSize.x > 0.0, quadSize.y > 0.0 else { return }
        while hoverEntities.count < rects.count {
            let entity = ModelEntity()
            entity.components.set(InputTargetComponent())
            quad.addChild(entity)
            hoverEntities.append(entity)
        }
        for (index, entity) in hoverEntities.enumerated() {
            guard index < rects.count else {
                entity.isEnabled = false
                continue
            }
            let rect = rects[index]
            let width = Float(rect.frame.width) / Float(kEyeWidth) * quadSize.x
            let height = Float(rect.frame.height) / Float(kTextureHeight) * quadSize.y
            guard width > 0.0, height > 0.0 else {
                entity.isEnabled = false
                continue
            }
            entity.isEnabled = true
            if rect.item {
                var window = material
                try? window.setParameter(name: "UVOffset",
                                         value: .simd2Float(SIMD2(Float(rect.frame.minX) / Float(kEyeWidth),
                                                                  1.0 - Float(rect.frame.maxY) / Float(kTextureHeight))))
                try? window.setParameter(name: "UVScale",
                                         value: .simd2Float(SIMD2(Float(rect.frame.width) / Float(kEyeWidth),
                                                                  Float(rect.frame.height) / Float(kTextureHeight))))
                entity.model = ModelComponent(mesh: .generatePlane(width: width, height: height,
                                                                   cornerRadius: min(0.004, 0.5 * min(width, height))),
                                              materials: [window])
                entity.components.set(HoverEffectComponent(.highlight(.init(color: .white, strength: 1.0))))
            } else {
                entity.components.remove(ModelComponent.self)
                entity.components.remove(HoverEffectComponent.self)
            }
            entity.position = SIMD3(Float(rect.frame.midX) / Float(kEyeWidth) * quadSize.x - 0.5 * quadSize.x,
                                    0.5 * quadSize.y - Float(rect.frame.midY) / Float(kTextureHeight) * quadSize.y,
                                    kHoverLift + Float(index) * kHoverStep)
            entity.components.set(CollisionComponent(shapes: [.generateBox(width: width, height: height,
                                                                           depth: 0.001)],
                                                     isStatic: true))
        }
    }

    func point(_ value: EntityTargetValue<DragGesture.Value>, pressed: Bool) {
        guard let quad, quadSize.x > 0.0, quadSize.y > 0.0 else { return }
        let local = value.convert(value.location3D, from: .local, to: quad)
        let u = min(max(local.x / quadSize.x + 0.5, 0.0), 1.0)
        let v = min(max(0.5 - local.y / quadSize.y, 0.0), 1.0)
        LighthouseVolumePoint(u * Float(kEyeWidth), v * Float(kTextureHeight), pressed)
    }

    private func quadLost(_ quad: ModelEntity) {
        guard phase == 2, quadLostAt == 0.0 else { return }
        quadLostAt = CACurrentMediaTime()
        note("the quad transform is gone: scene \(quad.scene != nil), "
             + "active \(quad.isActive), parent \(quad.parent != nil)")
    }

    private func quadFound() {
        guard quadLostAt != 0.0 else { return }
        let gone = CACurrentMediaTime() - quadLostAt
        quadLostAt = 0.0
        note("the quad transform is back after \(String(format: "%.1f", gone)) s")
    }

    func tick() {
        guard let quad else { return }
        readHover()
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
            lastQuadTransform = immersiveFromQuad
            quadFound()
        } else if let held = lastQuadTransform {
            frame.HasQuad = true
            frame.ImmersiveFromQuad = held
            quadLost(quad)
        } else {
            quadLost(quad)
        }
        LighthouseVolumeUpdate(frame)

        if let line = hoverNote, gCopyHandoff.copied {
            hoverNote = nil
            note(line)
        }

        guard gCopyHandoff.hasRoom else { return }
        let started = CACurrentMediaTime()
        guard let buffer = queue.makeCommandBuffer() else { return }
        gCopyHandoff.stash(buffer, texture.replace(using: buffer))
        LighthouseVolumeNotePrepare(CACurrentMediaTime() - started)
    }
}

private struct LighthouseVolumeView: View {
    let state: VolumeState
    @Environment(\.scenePhase) private var scenePhase
    @Environment(\.openImmersiveSpace) private var openImmersiveSpace
    @Environment(\.dismissImmersiveSpace) private var dismissImmersiveSpace

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
        .handlesGameControllerEvents(matching: .gamepad)
        .ornament(attachmentAnchor: .scene(.bottom), contentAlignment: .top) {
            Button("Menu") {
                LighthouseVolumeOpenMenu()
            }
            .glassBackgroundEffect()
            .padding(.top, kMenuGap)
        }
        .task {
            await state.loadEyeMaterial()
            _ = await state.session.run(.init(tracking: [.world]))

            gOpenSpace = openImmersiveSpace
            gDismissSpace = dismissImmersiveSpace
            LighthouseVolumeSetShutdownHandler({ leave() })

            let documents = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first
            let marker = documents?.appendingPathComponent("no_head_tracking")
            if let marker, FileManager.default.fileExists(atPath: marker.path) {
                try? FileManager.default.removeItem(at: marker)
                note("head tracking is held back for this run, so the head stands at the design range")
            } else {
                await holdSpace(true)
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
            Task { await holdSpace(phase == .active) }
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
        .defaultSize(width: kVolumeWidth, height: kVolumeHeight, depth: kVolumeDepth, in: .meters)
        .volumeWorldAlignment(.gravityAligned)

        ImmersiveSpace(id: kSpaceId) {
            RealityView { _ in }
        }
        .immersionStyle(selection: .constant(.mixed), in: .mixed)
    }
}

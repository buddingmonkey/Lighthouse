import Foundation
import GameController
import Metal
import QuartzCore
import RealityKit
import Spatial
import SwiftUI
import UIKit

private let kSpaceId = "LighthouseImmersiveSpace"
// More items than a menu ever has on screen at once.
private let kHoverRectMax = 256
private let kPlateSpace = "LighthousePlate"
private let kEyeWidth = 1280
private let kTextureHeight = 720
// The two eyes stand side by side in one picture, and a camera index switch in the material gives
// each eye its own half. RealityKit has no other way to draw a thing differently for each eye.
private let kTextureWidth = 2 * kEyeWidth

// The shutdown handler the bridge calls is a plain C function, so what it needs is here.
@MainActor private var gOpenSpace: OpenImmersiveSpaceAction?
@MainActor private var gDismissSpace: DismissImmersiveSpaceAction?
@MainActor private var gSpaceOpen = false
@MainActor private var gSpaceBusy = false
@MainActor private var gLeaving = false

// The immersive space is what makes ARKit answer, and only one app may hold one. Hold it while the
// volume is in use and give it back the moment it is not, or nothing else on the device can open
// content of its own.
@MainActor private func holdSpace(_ wanted: Bool) async {
    // The scene phase is reported before the actions are in hand, and there is nothing to do yet.
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

// Never end the process with the immersive space still open. visionOS then refuses to open content
// for any app, and the headset has to be restarted.
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
    FileHandle.standardError.write("Lighthouse volume: \(text)\n".data(using: .utf8)!)
}

// One place the wearer may look. The game texture pixels it covers, and whether it is an item or
// the window that hides the items behind it.
private struct HoverRect: Identifiable, Equatable {
    let id: Int
    let frame: CGRect
    let item: Bool
}

@MainActor @Observable private final class HoverPlate {
    var rects: [HoverRect] = []
    var size = SIMD2<Float>(0.0, 0.0)
}

// visionOS gives no app the gaze, so a highlight can only be drawn by the system. A clear view for
// each menu item, in front of the picture and the size of the item, is what the system needs to
// draw one. The views take no press, so the drag on the quad still carries every click.
private struct HoverPlateView: View {
    let plate: HoverPlate
    let press: (CGPoint, Bool) -> Void
    @PhysicalMetric(from: .meters) private var meter: CGFloat = 1.0

    var body: some View {
        let width = CGFloat(plate.size.x) * meter
        let height = CGFloat(plate.size.y) * meter
        // The whole game texture spans the whole quad, which is what the drag on the quad assumes
        // as well, so the two axes take their own scale.
        let across = width / CGFloat(kEyeWidth)
        let down = height / CGFloat(kTextureHeight)
        ZStack(alignment: .topLeading) {
            Color.clear
                .frame(width: max(width, 0.0), height: max(height, 0.0))
                .contentShape(Rectangle())
            ForEach(plate.rects) { rect in
                HoverPlace(item: rect.item)
                    .frame(width: rect.frame.width * across, height: rect.frame.height * down)
                    .offset(x: rect.frame.minX * across, y: rect.frame.minY * down)
            }
        }
        .frame(width: max(width, 0.0), height: max(height, 0.0), alignment: .topLeading)
        .coordinateSpace(name: kPlateSpace)
        .gesture(
            DragGesture(minimumDistance: 0.0, coordinateSpace: .named(kPlateSpace))
                .onChanged { press($0.location, true) }
                .onEnded { press($0.location, false) }
        )
    }
}

// Measured: a plate at opacity zero is not hit-testable, so it can never become active. The black
// anchor carries the alpha the gaze needs, and the group gives the gaze to the white flash.
private struct HoverPlace: View {
    let item: Bool

    var body: some View {
        ZStack {
            if item {
                RoundedRectangle(cornerRadius: 6.0, style: .continuous)
                    .fill(Color.white.opacity(0.25))
                    .contentShape(.hoverEffect, .rect(cornerRadius: 6.0))
                    .hoverEffect { effect, isActive, _ in
                        effect.opacity(isActive ? 1.0 : 0.0)
                    }
            }
            RoundedRectangle(cornerRadius: 6.0, style: .continuous)
                .fill(Color.black.opacity(0.25))
                .contentShape(.hoverEffect, .rect(cornerRadius: 6.0))
                .hoverEffect { effect, _, _ in
                    effect.opacity(0.08)
                }
        }
        .hoverEffectGroup()
    }
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
    let hover = HoverPlate()
    var pointsPerMeter: CGFloat = 1360.0
    private var rawHover = [LighthouseVolumeHoverRect](repeating: LighthouseVolumeHoverRect(), count: kHoverRectMax)

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
        let shape = ShapeResource.generateBox(width: quadSize.x, height: quadSize.y, depth: 0.002)
        quad?.components.set(CollisionComponent(shapes: [shape], isStatic: true))
    }

    // The game thread publishes the rectangles as it ends a frame. A menu that stands still gives
    // the same set every update, and SwiftUI is only asked to do the work when the set changes.
    private func readHover() {
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
        if hover.rects != next {
            hover.rects = next
        }
        if hover.size != quadSize {
            hover.size = quadSize
        }
    }

    // The plate stands in front of the quad, so the pinch lands there and not on the quad. It
    // reports the place in its own points, which is the picture itself, so the game texture pixel
    // is a scale away.
    func plate(_ location: CGPoint, pressed: Bool) {
        guard hover.size.x > 0.0, hover.size.y > 0.0 else { return }
        let width = CGFloat(hover.size.x) * CGFloat(pointsPerMeter)
        let height = CGFloat(hover.size.y) * CGFloat(pointsPerMeter)
        guard width > 0.0, height > 0.0 else { return }
        let x = min(max(location.x / width, 0.0), 1.0) * CGFloat(kEyeWidth)
        let y = min(max(location.y / height, 0.0), 1.0) * CGFloat(kTextureHeight)
        LighthouseVolumePoint(Float(x), Float(y), pressed)
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
    @PhysicalMetric(from: .meters) private var meter: CGFloat = 1.0
    @Environment(\.scenePhase) private var scenePhase
    @Environment(\.openImmersiveSpace) private var openImmersiveSpace
    @Environment(\.dismissImmersiveSpace) private var dismissImmersiveSpace

    var body: some View {
        GeometryReader3D { proxy in
            ZStack {
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

                // Measured: a volume puts a flat view at its front face and clips whatever stands in
                // front of that, and the picture hangs in the middle, so the plate is carried back.
                HoverPlateView(plate: state.hover) { location, pressed in
                    state.plate(location, pressed: pressed)
                }
                .offset(z: 0.004 * meter - proxy.size.depth * 0.5)
                .onAppear { state.pointsPerMeter = meter }
            }
        }
        // A volumetric window keeps the sticks for scrolling and the face buttons for itself, and
        // an app that says nothing gets the D pad, the stick clicks and Menu and nothing else. This
        // is what asks for the whole pad.
        .handlesGameControllerEvents(matching: .gamepad)
        .task {
            await state.loadEyeMaterial()
            _ = await state.session.run(.init(tracking: [.world]))

            // ARKit reports no head in the Shared Space. An empty mixed space beside the volume is
            // what makes the query answer; it draws nothing and hides nothing.
            gOpenSpace = openImmersiveSpace
            gDismissSpace = dismissImmersiveSpace
            LighthouseVolumeSetShutdownHandler({ leave() })

            // A file in Documents holds the immersive space back for one run, so the one unusual
            // thing this app does can be taken away with no build and no signing. It is taken away
            // as it is read, because devicectl can copy a file to the device and cannot remove one.
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
        .defaultSize(width: 1.0, height: 0.5625, depth: 0.35, in: .meters)
        .volumeWorldAlignment(.gravityAligned)

        ImmersiveSpace(id: kSpaceId) {
            RealityView { _ in }
        }
        .immersionStyle(selection: .constant(.mixed), in: .mixed)
    }
}

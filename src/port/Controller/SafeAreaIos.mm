#include "SafeAreaIos.h"

#import <UIKit/UIKit.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#include <cstring>
#include <imgui.h>
#include <spdlog/spdlog.h>

#if defined(__IPHONE_27_1) && __IPHONE_OS_VERSION_MAX_ALLOWED >= __IPHONE_27_1
#define LIGHTHOUSE_RESERVED_REGIONS 1
#endif

namespace {

constexpr CGFloat kSideCutoutTop = 0.25;
constexpr CGFloat kSideCutoutBottom = 0.75;

TouchControlsSafeArea sCached = {};

UIView* GameView() {
    const Uint32 id = (Uint32)(intptr_t)ImGui::GetMainViewport()->PlatformHandle;
    SDL_Window* window = id != 0 ? SDL_GetWindowFromID(id) : nullptr;
    if (window == nullptr) {
        window = SDL_GetKeyboardFocus();
    }
    if (window == nullptr) {
        return nil;
    }
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(window, &info) || info.subsystem != SDL_SYSWM_UIKIT || info.info.uikit.window == nil) {
        return nil;
    }
    UIWindow* uiWindow = info.info.uikit.window;
    UIView* view = uiWindow.rootViewController.view;
    return view != nil ? view : uiWindow;
}

void AddRect(TouchControlsKeepOut* list, int& count, CGRect rect, CGFloat height) {
    if (count >= kTouchControlsMaxKeepOuts || CGRectIsEmpty(rect)) {
        return;
    }
    list[count++] = { (float)(CGRectGetMinX(rect) / height), (float)(CGRectGetMinY(rect) / height),
                      (float)(CGRectGetMaxX(rect) / height), (float)(CGRectGetMaxY(rect) / height) };
}

void AddKeepOut(TouchControlsSafeArea& area, CGRect rect, CGFloat height) {
    AddRect(area.keepOuts, area.keepOutCount, rect, height);
}

bool AddReservedRegions(TouchControlsSafeArea& area, UIView* view, CGFloat height) {
#ifdef LIGHTHOUSE_RESERVED_REGIONS
    if (@available(iOS 27.1, *)) {
        NSArray<UIViewReservedRegion*>* regions =
            [view reservedRegionsOfKind:UIViewReservedRegionKind.occlusionRegionKind];
        for (UIViewReservedRegion* region in regions) {
            if (region.active) {
                AddKeepOut(area, region.frame, height);
            }
        }
        for (UIViewReservedRegion* region in [view reservedRegionsOfKind:UIViewReservedRegionKind.divisionRegionKind]) {
            if (region.active) {
                AddRect(area.divisions, area.divisionCount, region.frame, height);
            }
        }
        return regions.count > 0;
    }
#endif
    (void)area;
    (void)view;
    (void)height;
    return false;
}

void AddSideCutout(TouchControlsSafeArea& area, UIView* view, UIEdgeInsets in, CGFloat height) {
    const CGFloat width = view.bounds.size.width;
    bool leftSide = in.left > in.right;
    if (in.left == in.right) {
        const UIInterfaceOrientation orientation = view.window.windowScene.effectiveGeometry.interfaceOrientation;
        leftSide = orientation == UIInterfaceOrientationLandscapeRight;
    }
    const CGFloat depth = MAX(in.left, in.right);
    const CGFloat x = leftSide ? 0.0 : width - depth;
    AddKeepOut(area, CGRectMake(x, height * kSideCutoutTop, depth, height * (kSideCutoutBottom - kSideCutoutTop)),
               height);
}

} // namespace

TouchControlsSafeArea TouchControls_IosSafeArea() {
    if (!NSThread.isMainThread) {
        return sCached;
    }
    UIView* view = GameView();
    const CGSize size = view.bounds.size;
    if (view == nil || size.height <= 0.0) {
        return sCached;
    }
    const UIEdgeInsets in = view.safeAreaInsets;
    TouchControlsSafeArea area = {};
    const bool landscape = size.width > size.height;
    if (!landscape) {
        area.top = (float)(in.top / size.height);
        area.bottom = (float)(in.bottom / size.height);
    }
    if (!AddReservedRegions(area, view, size.height) && landscape && MAX(in.left, in.right) > 0.0) {
        AddSideCutout(area, view, in, size.height);
    }
    if (std::memcmp(&area, &sCached, sizeof(area)) != 0) {
        SPDLOG_INFO("Touch safe area: view {}x{}, insets l{} t{} r{} b{}, {} keep-out(s), {} division(s)", size.width,
                    size.height, in.left, in.top, in.right, in.bottom, area.keepOutCount, area.divisionCount);
        for (int i = 0; i < area.keepOutCount; i++) {
            const TouchControlsKeepOut& k = area.keepOuts[i];
            SPDLOG_INFO("Touch keep-out {}: {:.3f},{:.3f} - {:.3f},{:.3f}", i, k.left, k.top, k.right, k.bottom);
        }
        for (int i = 0; i < area.divisionCount; i++) {
            const TouchControlsKeepOut& d = area.divisions[i];
            SPDLOG_INFO("Touch division {}: {:.3f},{:.3f} - {:.3f},{:.3f}", i, d.left, d.top, d.right, d.bottom);
        }
    }
    sCached = area;
    return sCached;
}

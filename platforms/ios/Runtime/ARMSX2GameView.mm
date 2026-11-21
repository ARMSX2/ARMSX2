// ARMSX2GameView.mm — the game render view and its Metal layer
// SPDX-License-Identifier: GPL-3.0+

#import <UIKit/UIKit.h>
#import <QuartzCore/CAMetalLayer.h>

#include <algorithm>

#include "pcsx2/Host.h"
#include "pcsx2/GS/GS.h"
#include "pcsx2/ImGui/ImGuiManager.h"

#import "ARMSX2GameView.h"

#pragma mark - ARMSX2GameView
@implementation ARMSX2GameView
+ (Class)layerClass { return [CAMetalLayer class]; }
- (instancetype)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (self)
        [self armsx2ApplyNativeContentScale];
    return self;
}
- (void)didMoveToWindow {
    [super didMoveToWindow];
    [self armsx2ApplyNativeContentScale];
    [self setNeedsLayout];
}
- (CGFloat)armsx2NativeContentScale {
    UIScreen* screen = self.window.screen ?: UIScreen.mainScreen;
    CGFloat scale = screen.nativeScale > 0.0 ? screen.nativeScale : screen.scale;
    return scale > 0.0 ? scale : 1.0;
}
- (void)armsx2ApplyNativeContentScale {
    const CGFloat scale = [self armsx2NativeContentScale];
    self.contentScaleFactor = scale;
    self.layer.contentsScale = scale;
    ((CAMetalLayer*)self.layer).contentsScale = scale;
}
- (void)layoutSubviews {
    [super layoutSubviews];
    [self armsx2ApplyNativeContentScale];
    if (self.bounds.size.width <= 0.0 || self.bounds.size.height <= 0.0)
        return;

    CGFloat scale = self.contentScaleFactor;
    CAMetalLayer *mtl = (CAMetalLayer *)self.layer;
    mtl.drawableSize = CGSizeMake(self.bounds.size.width * scale,
                                   self.bounds.size.height * scale);
    int w = std::max(1, (int)(self.bounds.size.width * scale + 0.5));
    int h = std::max(1, (int)(self.bounds.size.height * scale + 0.5));
    float s = (float)scale;

    // Keep corner-anchored OSD out of the notch, the Dynamic Island and the home indicator, using
    // UIKit's safe area: no flat inset fits both square corners and a cut-out.
    const UIEdgeInsets safe = self.safeAreaInsets;
    ImGuiManager::SetOSDSafeAreaInsets((float)(safe.left * scale), (float)(safe.top * scale),
                                       (float)(safe.right * scale), (float)(safe.bottom * scale));

    // Runs on the main thread, but the MTGS ring's one producer is the CPU thread, so hop through
    // Host::RunOnGSThread. The insets go direct: that hop drops work while the GS thread is closed.
    Host::RunOnGSThread([w, h, s]() {
        GSResizeDisplayWindow(w, h, s);
    });
}
@end

// ARMSX2ExternalDisplaySceneDelegate.mm — the AirPlay external display scene
// SPDX-License-Identifier: GPL-3.0+

#import <UIKit/UIKit.h>
#import <GameController/GameController.h>
#import <MetalKit/MetalKit.h>

#include "common/Console.h"

#import "ARMSX2iOS-Swift.h"
#import "PCSX2SceneDelegate.h"

@implementation ARMSX2ExternalDisplaySceneDelegate

- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)connectionOptions {
    if (![scene isKindOfClass:[UIWindowScene class]]) return;

    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    self.window.backgroundColor = UIColor.blackColor;
    self.window.rootViewController = [SwiftUIHost createExternalDisplayController];
    [self.window makeKeyAndVisible];
    [SwiftUIHost setExternalDisplayConnected:YES];
    Console.WriteLn("[AirPlay] External display connected");
}

- (void)sceneDidDisconnect:(UIScene *)scene {
    [SwiftUIHost setExternalDisplayConnected:NO];
    Console.WriteLn("[AirPlay] External display disconnected");
    self.window = nil;
}

@end

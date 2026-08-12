#pragma once
#import <UIKit/UIKit.h>

@interface PCSX2SceneDelegate : UIResponder <UIWindowSceneDelegate, UIDocumentPickerDelegate>
@property (strong, nonatomic) UIWindow *window;
@property (strong, nonatomic) UIButton *startBiosButton;
@end

// External-display scenes must never run the SDL/VM bootstrap performed by
// PCSX2SceneDelegate. They only own the UIKit window which hosts the existing
// Metal renderer.
@interface PCSX2ExternalDisplaySceneDelegate : UIResponder <UIWindowSceneDelegate>
@property (strong, nonatomic) UIWindow *window;
@end

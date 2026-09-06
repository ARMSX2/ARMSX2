#pragma once
#import <UIKit/UIKit.h>

@interface PCSX2SceneDelegate : UIResponder <UIWindowSceneDelegate, UIDocumentPickerDelegate>
@property (strong, nonatomic) UIWindow *window;
@property (strong, nonatomic) UIButton *startBiosButton;
@end

@interface ARMSX2ExternalDisplaySceneDelegate : UIResponder <UIWindowSceneDelegate>
@property (strong, nonatomic) UIWindow *window;
@end

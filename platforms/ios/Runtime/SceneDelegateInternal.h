// SceneDelegateInternal.h — what SceneDelegate.mm and its VM thread category share
#pragma once

#import "PCSX2SceneDelegate.h"

#include <atomic>

extern std::atomic<bool> s_loadLastSaveStateOnBoot;
bool ARMSX2JITWorkerBusy();

@interface PCSX2SceneDelegate (VMThread)
- (void)checkJITAndStartVM;
- (void)startVMThread;
- (void)prepareVMThreadForIdle;
@end

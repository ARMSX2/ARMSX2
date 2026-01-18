//
//  JITManager.h
//  ARMSX2
//
//  JIT Memory Manager for iOS 26
//  Handles dynamic code generation and memory allocation for the PS2 emulator
//

#import <Foundation/Foundation.h>
#import <mach/mach.h>
#import <pthread.h>

NS_ASSUME_NONNULL_BEGIN

typedef NS_ENUM(NSInteger, JITStatus) {
    JITStatusUnknown = 0,
    JITStatusAvailable = 1,
    JITStatusUnavailable = 2,
    JITStatusEnabled = 3,
    JITStatusFailed = 4
};

@interface JITManager : NSObject

/// Shared singleton instance
@property (class, readonly, nonatomic) JITManager *sharedManager;

/// Current JIT status
@property (readonly, nonatomic) JITStatus status;

/// Whether JIT is currently enabled
@property (readonly, nonatomic) BOOL isJITEnabled;

/// Total JIT memory allocated (in bytes)
@property (readonly, nonatomic) size_t totalJITMemory;

/// Initialize and check JIT availability on iOS 26
- (BOOL)initializeJIT;

/// Enable JIT compilation for the current thread
- (BOOL)enableJITOnCurrentThread;

/// Disable JIT compilation for the current thread
- (void)disableJITOnCurrentThread;

/// Allocate executable memory for JIT code
/// @param size Size of memory to allocate in bytes
/// @return Pointer to allocated executable memory, or NULL on failure
- (void * _Nullable)allocateJITMemory:(size_t)size;

/// Free JIT-allocated memory
/// @param pointer Pointer to memory allocated by allocateJITMemory
/// @param size Size of the allocation
- (void)freeJITMemory:(void *)pointer size:(size_t)size;

/// Make memory executable (mark as JIT code region)
/// @param pointer Pointer to memory region
/// @param size Size of the region
/// @return YES if successful, NO otherwise
- (BOOL)makeMemoryExecutable:(void *)pointer size:(size_t)size;

/// Make memory writable (for modifying JIT code)
/// @param pointer Pointer to memory region
/// @param size Size of the region
/// @return YES if successful, NO otherwise
- (BOOL)makeMemoryWritable:(void *)pointer size:(size_t)size;

/// Flush instruction cache for JIT-compiled code
/// @param pointer Pointer to code region
/// @param size Size of the region
- (void)flushInstructionCache:(void *)pointer size:(size_t)size;

/// Request iOS 26 JIT permission from the system
/// This uses iOS 26's new JIT APIs
- (BOOL)requestJITPermission;

/// Get detailed JIT status information
- (NSString *)statusDescription;

@end

NS_ASSUME_NONNULL_END

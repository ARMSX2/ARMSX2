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

// ============================================================================
// Fast JIT Toggle Macros (DolphinOS-style)
// ============================================================================
// These inline macros provide minimal overhead for JIT write protection toggle
// Use these in performance-critical code paths (e.g., JIT compiler hot loops)

#if defined(__cplusplus)
extern "C" {
#endif

// Fast JIT write enable (allows writing to JIT memory)
// Note: This is thread-local. Each thread must call this independently.
static inline void ARMSX2_JIT_EnableWrite(void) {
    if (__builtin_available(iOS 14.0, *)) {
        extern int pthread_jit_write_protect_np(int enabled);
        pthread_jit_write_protect_np(0);
    }
}

// Fast JIT write disable (makes JIT memory executable)
// Note: This is thread-local. Each thread must call this independently.
static inline void ARMSX2_JIT_DisableWrite(void) {
    if (__builtin_available(iOS 14.0, *)) {
        extern int pthread_jit_write_protect_np(int enabled);
        pthread_jit_write_protect_np(1);
    }
}

// Fast instruction cache invalidation
// Call after writing code to ensure CPU sees the new instructions
static inline void ARMSX2_JIT_FlushCache(void* addr, size_t len) {
    extern void sys_icache_invalidate(void *start, size_t len);
    sys_icache_invalidate(addr, len);
}

#if defined(__cplusplus)
}
#endif

// ============================================================================
// Convenience macros for common JIT write patterns
// ============================================================================

// Write code to JIT memory with automatic protection toggle
#define ARMSX2_JIT_WRITE_CODE(dest, src, len) do { \
    ARMSX2_JIT_EnableWrite(); \
    memcpy(dest, src, len); \
    ARMSX2_JIT_DisableWrite(); \
    ARMSX2_JIT_FlushCache(dest, len); \
} while(0)

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

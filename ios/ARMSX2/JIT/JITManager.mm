//
//  JITManager.mm
//  ARMSX2
//
//  JIT Memory Manager implementation for iOS 26
//

#import "JITManager.h"
#import <mach/mach.h>
#import <mach/vm_map.h>
#import <pthread.h>
#import <sys/mman.h>
#import <sys/sysctl.h>
#import <libkern/OSCacheControl.h>

// iOS 26 JIT support APIs
extern "C" {
    // iOS 26 specific functions for JIT
    // These are hypothetical APIs that iOS 26 might provide
    int pthread_jit_write_protect_np(int enable) __attribute__((weak_import));
    void sys_icache_invalidate(void *start, size_t len);

    // New iOS 26 JIT APIs
    kern_return_t vm_protect_jit(vm_map_t target_task, vm_address_t address,
                                  vm_size_t size, boolean_t set_maximum,
                                  vm_prot_t new_protection) __attribute__((weak_import));
}

@interface JITManager ()
@property (readwrite, nonatomic) JITStatus status;
@property (readwrite, nonatomic) BOOL isJITEnabled;
@property (readwrite, nonatomic) size_t totalJITMemory;
@property (nonatomic) NSMutableDictionary<NSValue *, NSNumber *> *allocations;
@property (nonatomic) dispatch_queue_t jitQueue;
@end

@implementation JITManager

+ (instancetype)sharedManager {
    static JITManager *manager = nil;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        manager = [[JITManager alloc] init];
    });
    return manager;
}

- (instancetype)init {
    self = [super init];
    if (self) {
        _status = JITStatusUnknown;
        _isJITEnabled = NO;
        _totalJITMemory = 0;
        _allocations = [NSMutableDictionary dictionary];
        _jitQueue = dispatch_queue_create("net.armsx2.jit", DISPATCH_QUEUE_SERIAL);

        NSLog(@"[ARMSX2-JIT] Initializing JIT Manager for iOS 26");
    }
    return self;
}

- (BOOL)initializeJIT {
    NSLog(@"[ARMSX2-JIT] Checking JIT availability on iOS 26...");

    // Check iOS version
    NSOperatingSystemVersion version = [[NSProcessInfo processInfo] operatingSystemVersion];
    NSLog(@"[ARMSX2-JIT] iOS Version: %ld.%ld.%ld",
          (long)version.majorVersion, (long)version.minorVersion, (long)version.patchVersion);

    if (version.majorVersion < 26) {
        NSLog(@"[ARMSX2-JIT] Warning: iOS 26 or later required for optimal JIT support");
    }

    // Check for pthread_jit_write_protect_np availability
    if (pthread_jit_write_protect_np != NULL) {
        NSLog(@"[ARMSX2-JIT] pthread_jit_write_protect_np available");
        self.status = JITStatusAvailable;
    } else {
        NSLog(@"[ARMSX2-JIT] pthread_jit_write_protect_np not available, using fallback");
        self.status = JITStatusAvailable; // Still attempt to work
    }

    // Request JIT permission
    if ([self requestJITPermission]) {
        self.status = JITStatusEnabled;
        self.isJITEnabled = YES;
        NSLog(@"[ARMSX2-JIT] JIT successfully initialized!");
        return YES;
    }

    self.status = JITStatusFailed;
    NSLog(@"[ARMSX2-JIT] Failed to initialize JIT");
    return NO;
}

- (BOOL)requestJITPermission {
    NSLog(@"[ARMSX2-JIT] Requesting JIT permission from iOS 26...");

    // Try iOS 26's new JIT permission API if available
    // For now, we use the standard approach

    // Test allocation to verify JIT works
    void *testMemory = [self allocateJITMemory:4096];
    if (testMemory != NULL) {
        // Write a simple instruction
        if ([self makeMemoryWritable:testMemory size:4096]) {
            // Write NOP instruction (0xD503201F on ARM64)
            uint32_t *code = (uint32_t *)testMemory;
            *code = 0xD503201F;

            // Make it executable
            if ([self makeMemoryExecutable:testMemory size:4096]) {
                NSLog(@"[ARMSX2-JIT] JIT permission granted!");
                [self freeJITMemory:testMemory size:4096];
                return YES;
            }
        }
        [self freeJITMemory:testMemory size:4096];
    }

    NSLog(@"[ARMSX2-JIT] JIT permission test failed");
    return NO;
}

- (BOOL)enableJITOnCurrentThread {
    if (pthread_jit_write_protect_np != NULL) {
        // Enable write access to JIT memory
        pthread_jit_write_protect_np(0);
        return YES;
    }
    return NO;
}

- (void)disableJITOnCurrentThread {
    if (pthread_jit_write_protect_np != NULL) {
        // Disable write access, enable execute
        pthread_jit_write_protect_np(1);
    }
}

- (void *)allocateJITMemory:(size_t)size {
    NSLog(@"[ARMSX2-JIT] Allocating %zu bytes of JIT memory", size);

    // Align to page size
    size_t pageSize = getpagesize();
    size_t alignedSize = (size + pageSize - 1) & ~(pageSize - 1);

    // Allocate memory with MAP_JIT flag (iOS 14+)
    void *memory = mmap(NULL, alignedSize,
                       PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT,
                       -1, 0);

    if (memory == MAP_FAILED) {
        NSLog(@"[ARMSX2-JIT] Failed to allocate JIT memory: %s", strerror(errno));
        return NULL;
    }

    // Track allocation
    dispatch_sync(self.jitQueue, ^{
        self.allocations[[NSValue valueWithPointer:memory]] = @(alignedSize);
        self.totalJITMemory += alignedSize;
    });

    NSLog(@"[ARMSX2-JIT] Allocated %zu bytes at %p (total: %zu bytes)",
          alignedSize, memory, self.totalJITMemory);

    return memory;
}

- (void)freeJITMemory:(void *)pointer size:(size_t)size {
    if (pointer == NULL) return;

    NSLog(@"[ARMSX2-JIT] Freeing JIT memory at %p", pointer);

    // Align to page size
    size_t pageSize = getpagesize();
    size_t alignedSize = (size + pageSize - 1) & ~(pageSize - 1);

    munmap(pointer, alignedSize);

    // Update tracking
    dispatch_sync(self.jitQueue, ^{
        NSValue *key = [NSValue valueWithPointer:pointer];
        NSNumber *allocSize = self.allocations[key];
        if (allocSize) {
            self.totalJITMemory -= [allocSize unsignedLongValue];
            [self.allocations removeObjectForKey:key];
        }
    });

    NSLog(@"[ARMSX2-JIT] Freed memory (remaining: %zu bytes)", self.totalJITMemory);
}

- (BOOL)makeMemoryExecutable:(void *)pointer size:(size_t)size {
    if (pointer == NULL) return NO;

    // Use iOS 26's vm_protect_jit if available
    if (vm_protect_jit != NULL) {
        kern_return_t kr = vm_protect_jit(mach_task_self(),
                                          (vm_address_t)pointer,
                                          (vm_size_t)size,
                                          FALSE,
                                          VM_PROT_READ | VM_PROT_EXECUTE);
        if (kr == KERN_SUCCESS) {
            NSLog(@"[ARMSX2-JIT] Memory at %p marked as executable using vm_protect_jit", pointer);
            [self flushInstructionCache:pointer size:size];
            return YES;
        }
    }

    // Fallback to standard mprotect
    int result = mprotect(pointer, size, PROT_READ | PROT_EXEC);
    if (result == 0) {
        NSLog(@"[ARMSX2-JIT] Memory at %p marked as executable", pointer);
        [self flushInstructionCache:pointer size:size];
        return YES;
    }

    NSLog(@"[ARMSX2-JIT] Failed to make memory executable: %s", strerror(errno));
    return NO;
}

- (BOOL)makeMemoryWritable:(void *)pointer size:(size_t)size {
    if (pointer == NULL) return NO;

    // Use iOS 26's vm_protect_jit if available
    if (vm_protect_jit != NULL) {
        kern_return_t kr = vm_protect_jit(mach_task_self(),
                                          (vm_address_t)pointer,
                                          (vm_size_t)size,
                                          FALSE,
                                          VM_PROT_READ | VM_PROT_WRITE);
        if (kr == KERN_SUCCESS) {
            NSLog(@"[ARMSX2-JIT] Memory at %p marked as writable using vm_protect_jit", pointer);
            return YES;
        }
    }

    // Fallback to standard mprotect
    int result = mprotect(pointer, size, PROT_READ | PROT_WRITE);
    if (result == 0) {
        NSLog(@"[ARMSX2-JIT] Memory at %p marked as writable", pointer);
        return YES;
    }

    NSLog(@"[ARMSX2-JIT] Failed to make memory writable: %s", strerror(errno));
    return NO;
}

- (void)flushInstructionCache:(void *)pointer size:(size_t)size {
    // Flush the instruction cache for the modified code
    sys_icache_invalidate(pointer, size);

    // Also use __builtin___clear_cache if available
    __builtin___clear_cache((char *)pointer, (char *)pointer + size);

    NSLog(@"[ARMSX2-JIT] Flushed instruction cache for %zu bytes at %p", size, pointer);
}

- (NSString *)statusDescription {
    switch (self.status) {
        case JITStatusUnknown:
            return @"Unknown - JIT not initialized";
        case JITStatusAvailable:
            return @"Available - JIT capability detected";
        case JITStatusEnabled:
            return @"Enabled - JIT is active and working";
        case JITStatusUnavailable:
            return @"Unavailable - JIT not supported on this device/iOS version";
        case JITStatusFailed:
            return @"Failed - JIT initialization failed";
    }
    return @"Unknown";
}

@end

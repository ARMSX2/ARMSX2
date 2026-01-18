# ARMSX2 JIT Usage Guide for iOS

## Overview

ARMSX2 provides two approaches for JIT memory management on iOS:

1. **Fast Path** (DolphinOS-style): Minimal overhead inline macros for hot code paths
2. **Managed Path**: Full-featured methods with tracking and diagnostics

## Fast Path API (Recommended for Performance)

### Basic Usage

```objc
#import "JITManager.h"

// In your JIT compiler hot loop:
void CompileBlock(void* jit_buffer, const uint8_t* code, size_t size) {
    // Enable writing to JIT memory
    ARMSX2_JIT_EnableWrite();

    // Write compiled code
    memcpy(jit_buffer, code, size);

    // Make memory executable
    ARMSX2_JIT_DisableWrite();

    // Flush instruction cache
    ARMSX2_JIT_FlushCache(jit_buffer, size);
}
```

### One-Liner Convenience Macro

For simple code writes:

```objc
// Automatically handles enable/disable/flush
ARMSX2_JIT_WRITE_CODE(dest_ptr, source_ptr, length);
```

### Example: PCSX2 Recompiler Integration

```cpp
// In PCSX2's x64 recompiler (for ARM64 iOS)
namespace Dynarec {

void EmitBlock(u32 pc) {
    // Allocate JIT buffer (once per block)
    void* code_ptr = [[JITManager sharedManager] allocateJITMemory:4096];

    // Generate ARM64 instructions
    ARM64CodeBuffer buffer;
    TranslateBlock(pc, &buffer);

    // Fast path: Write to JIT memory
    ARMSX2_JIT_EnableWrite();
    memcpy(code_ptr, buffer.GetCode(), buffer.GetSize());
    ARMSX2_JIT_DisableWrite();
    ARMSX2_JIT_FlushCache(code_ptr, buffer.GetSize());

    // Execute the block
    ((void(*)())code_ptr)();
}

} // namespace Dynarec
```

## Managed Path API (Full Features)

### When to Use

- First-time initialization
- Memory allocation/deallocation
- Diagnostic logging needed
- Debug builds

### Basic Usage

```objc
JITManager* jit = [JITManager sharedManager];

// Initialize (call once at app start)
if (![jit initializeJIT]) {
    NSLog(@"JIT not available on this device");
    return;
}

// Allocate memory for JIT code
void* code_buffer = [jit allocateJITMemory:4096];

// Make writable for code generation
[jit makeMemoryWritable:code_buffer size:4096];

// Write your code
memcpy(code_buffer, compiled_code, code_size);

// Make executable
[jit makeMemoryExecutable:code_buffer size:4096];

// Flush instruction cache
[jit flushInstructionCache:code_buffer size:code_size];

// Later: free when done
[jit freeJITMemory:code_buffer size:4096];
```

### Example: PCSX2 EE Recompiler

```cpp
// In PCSX2's EE (Emotion Engine) recompiler
namespace EE::Dynarec {

class X64Emitter {
private:
    void* m_code_buffer;
    size_t m_code_size;
    JITManager* m_jit_manager;

public:
    X64Emitter() {
        m_jit_manager = [JITManager sharedManager];

        // Allocate 1MB for JIT code cache
        m_code_buffer = [m_jit_manager allocateJITMemory:1024*1024];
        m_code_size = 0;
    }

    ~X64Emitter() {
        [m_jit_manager freeJITMemory:m_code_buffer size:1024*1024];
    }

    void* CompileBlock(u32 pc, u32 length) {
        void* block_start = (uint8_t*)m_code_buffer + m_code_size;

        // Fast path for writing
        ARMSX2_JIT_EnableWrite();

        // Generate code directly
        EmitProlog();
        for (u32 i = 0; i < length; i++) {
            TranslateInstruction(Memory::Read32(pc + i*4));
        }
        EmitEpilog();

        size_t block_size = GetCodeSize();

        ARMSX2_JIT_DisableWrite();
        ARMSX2_JIT_FlushCache(block_start, block_size);

        m_code_size += block_size;
        return block_start;
    }
};

} // namespace EE::Dynarec
```

## Performance Comparison

### Fast Path
```cpp
// ~10ns overhead per toggle
ARMSX2_JIT_EnableWrite();   // pthread_jit_write_protect_np(0)
memcpy(dest, src, size);
ARMSX2_JIT_DisableWrite();  // pthread_jit_write_protect_np(1)
ARMSX2_JIT_FlushCache(dest, size);
```

### Managed Path
```cpp
// ~50ns overhead per toggle (method call + null checks)
[[JITManager sharedManager] enableJITOnCurrentThread];
memcpy(dest, src, size);
[[JITManager sharedManager] disableJITOnCurrentThread];
[[JITManager sharedManager] flushInstructionCache:dest size:size];
```

**Recommendation**: Use **fast path** in hot loops (compile functions), **managed path** for setup/teardown.

## Thread Safety

### Fast Path
```objc
// Thread-local! Each thread must enable independently.
dispatch_queue_t compile_queue = dispatch_queue_create("jit.compiler", DISPATCH_QUEUE_CONCURRENT);

dispatch_async(compile_queue, ^{
    // Enable JIT for THIS thread
    ARMSX2_JIT_EnableWrite();

    // Compile code
    CompileBlock(...);

    // Disable for THIS thread
    ARMSX2_JIT_DisableWrite();
});
```

### Important Notes
- `pthread_jit_write_protect_np` is **per-thread**
- Each compilation thread must enable/disable independently
- Don't enable on one thread and disable on another!

## Debug vs Release Builds

### Debug Build
- Full memory tracking enabled
- Detailed logging
- Leak detection
- Allocation statistics

### Release Build
- Zero tracking overhead
- Minimal logging (errors only)
- Maximum performance

Configure in Xcode build settings:
- Debug: `-DDEBUG=1`
- Release: (no DEBUG flag)

## Common Patterns

### Pattern 1: One-Time Setup
```objc
- (void)setupJIT {
    JITManager* jit = [JITManager sharedManager];

    // Initialize (managed path)
    if (![jit initializeJIT]) {
        @throw [NSException exceptionWithName:@"JITError"
                                       reason:@"JIT not available"
                                     userInfo:nil];
    }

    NSLog(@"JIT Status: %@", [jit statusDescription]);
}
```

### Pattern 2: Compile Loop
```objc
- (void)compileAllBlocks {
    // Enable once for the loop
    ARMSX2_JIT_EnableWrite();

    for (Block* block in self.blocks) {
        // Write code (already enabled)
        memcpy(block->jit_ptr, block->compiled_code, block->size);
    }

    // Disable once after loop
    ARMSX2_JIT_DisableWrite();

    // Flush all blocks
    for (Block* block in self.blocks) {
        ARMSX2_JIT_FlushCache(block->jit_ptr, block->size);
    }
}
```

### Pattern 3: Lazy Compilation
```objc
- (void)executeBlock:(u32)pc {
    void* jit_block = [self lookupBlock:pc];

    if (!jit_block) {
        // Compile on demand (fast path)
        jit_block = [self compileBlock:pc];
        [self cacheBlock:pc ptr:jit_block];
    }

    // Execute
    ((void(*)(void))jit_block)();
}
```

## iOS 26 Enhancements

### vm_protect_jit API

The JITManager automatically uses iOS 26's new `vm_protect_jit` when available:

```objc
- (BOOL)makeMemoryExecutable:(void*)ptr size:(size_t)size {
    // Tries iOS 26 API first
    if (vm_protect_jit != NULL) {
        kern_return_t kr = vm_protect_jit(mach_task_self(), ptr, size,
                                          FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
        if (kr == KERN_SUCCESS) {
            return YES;
        }
    }

    // Falls back to iOS 14+ mprotect
    return mprotect(ptr, size, PROT_READ | PROT_EXEC) == 0;
}
```

This happens automatically - no code changes needed!

## Debugging JIT Issues

### Check JIT Status
```objc
JITManager* jit = [JITManager sharedManager];
NSLog(@"Status: %@", [jit statusDescription]);
NSLog(@"Enabled: %d", jit.isJITEnabled);
NSLog(@"Memory: %zu bytes", jit.totalJITMemory); // DEBUG only
```

### Common Issues

**1. Crash when executing JIT code**
```
Cause: Forgot to disable write protection
Fix: Call ARMSX2_JIT_DisableWrite() before execution
```

**2. Wrong instructions executed**
```
Cause: Instruction cache not flushed
Fix: Call ARMSX2_JIT_FlushCache() after writing code
```

**3. JIT not available**
```
Cause: iOS < 14, or entitlements not configured
Fix: Check iOS version, verify entitlements in Xcode
```

**4. Thread-specific crash**
```
Cause: Enabled on one thread, disabled on another
Fix: Each thread must call enable/disable independently
```

### Verification Test

```objc
- (void)testJIT {
    JITManager* jit = [JITManager sharedManager];

    // Allocate
    void* code = [jit allocateJITMemory:4096];
    assert(code != NULL);

    // Write a simple ARM64 NOP (0xD503201F)
    ARMSX2_JIT_EnableWrite();
    *(uint32_t*)code = 0xD503201F;
    ARMSX2_JIT_DisableWrite();
    ARMSX2_JIT_FlushCache(code, 4);

    // Execute (should do nothing, but not crash)
    ((void(*)(void))code)();

    // Cleanup
    [jit freeJITMemory:code size:4096];

    NSLog(@"JIT test passed!");
}
```

## Best Practices

1. **Use fast path in hot loops** (compiling individual instructions)
2. **Use managed path for allocation** (setup/teardown)
3. **Always flush cache** after writing code
4. **Keep write protection disabled minimal time** (security)
5. **One enable/disable per compile block** (don't toggle per instruction)
6. **Don't mix threads** (each thread manages its own state)
7. **Don't keep write enabled** (violates W^X)
8. **Don't skip cache flush** (undefined behavior)

## Integration Checklist

- [ ] Import `JITManager.h` in your JIT compiler
- [ ] Initialize JIT at app startup
- [ ] Allocate code buffer with `allocateJITMemory:`
- [ ] Use `ARMSX2_JIT_WRITE_CODE` or manual enable/disable
- [ ] Always flush cache after writing
- [ ] Free memory when done
- [ ] Test on real device (simulator doesn't enforce JIT)
- [ ] Verify entitlements are configured

## References

- DolphinOS JIT: https://github.com/OatmealDome/dolphin-ios
- Apple W^X: https://developer.apple.com/security/
- iOS JIT Guide: ios/JIT_COMPARISON.md

---

**Need Help?**
- Check logs: `[ARMSX2-JIT]` prefix
- Verify status: `[[JITManager sharedManager] statusDescription]`
- See examples: This file, `JITManager.mm`

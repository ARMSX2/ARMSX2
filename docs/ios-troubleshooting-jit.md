# ARMSX2 iOS — JIT troubleshooting

## Black screen on boot (v2.2.2, iOS 26.x)

### Symptoms

- ARMSX2 opens and JIT appears enabled (`CS_DEBUGGED=1`, `LuckTXM`, `@@BOOT_JIT_GATE@@ available=1`).
- Booting the PS2 BIOS or a game shows a **permanent black screen**.
- The in-app UI may still respond, but no BIOS animation, menu, or game video appears.
- Logs may stop after:

```txt
@@JIT_ALLOC@@ txm_protocol=universal
@@JIT_ALLOC@@ txm_register_universal_begin rx=... size=0xa100000
```

Related report: [GitHub issue #200](https://github.com/ARMSX2/ARMSX2/issues/200).

### Confirmed workaround (iOS 26.5)

Switch the JIT script inside ARMSX2:

1. **Settings → Emulator → JIT Script → Legacy**
2. Fully close and reopen the app.

**Tested working:**

- iPhone 15 Plus — iOS 26.5
- iPhone 17 — iOS 26.5

With **Legacy**, BIOS and games boot normally. With **Universal** (the v2.2.2 default on iOS 26+), TXM registration can hang on large code regions under LuckTXM.

### Why Legacy vs Universal?

- **Universal** — StikDebug `brk #0xf00d` prepare + detach. Default on iOS 26+ in v2.2.2; can hang during ~161 MB code registration.
- **Legacy** — `brk #0x69` TXM registration. Reliable workaround on tested iOS 26.5 devices (iPhone 15 Plus, iPhone 17).

Code memory registration runs **once** when the VM thread is first created. After changing JIT Script, a **full app restart** is required.

### Version context

- **v1.3.2** — Different JIT/memory path; some titles (e.g. Kingdom Hearts II) did not run well.
- **v2.2.2 + Legacy JIT** — Improved compatibility; KH2 and other titles run well on tested hardware when Legacy is selected.

### Diagnostic environment variables (advanced)

- `ARMSX2_JIT_PROTOCOL=legacy` or `universal` — Force JIT script protocol
- `ARMSX2_TXM_REGISTER_TIMEOUT_MS=8000` — Timeout for Universal registration before fallback
- `ARMSX2_VM_INIT_TIMEOUT_MS=15000` — VM init watchdog (shows error instead of silent black screen)
- `ARMSX2_ALLOW_NO_TXM=1` — Diagnostic: skip TXM registration (not for normal use)

### If Legacy still fails

1. Confirm JIT is enabled (StikDebug / sideload setup marks the process as debugged).
2. Check `Documents/pcsx2_log.txt` for `@@BOOT_FAIL@@` or `code_dualmap_fail`.
3. Open or update [issue #200](https://github.com/ARMSX2/ARMSX2/issues/200) with device model, iOS version, JIT Script setting, and log excerpts (no BIOS/ISO files).

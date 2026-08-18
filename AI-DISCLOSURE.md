# Read this before reviewing

This issue or PR was made via an AI agent and likely has not been reviewed by a human at all,
your time may be entirely wasted.

That sentence is required by `AGENTS.md` in this repository, and it is accurate: no person has
read this diff line by line. What follows is what was and was not checked, so you can decide
where to spend attention.

## Checked

The 867-preset catalogue was built, published and fetched back over the real URL, and one zip
was hashed against the manifest's own SHA-256. A download was installed through the app in the
iOS simulator and the pack landed with its tree intact, a version marker, and no staging file
left behind. A patched shader was compiled and rendered at 3x internal resolution, the setting
that used to turn the frame black, and measured at 60/255 mean luminance rather than eyeballed.
Offline browsing was confirmed by pointing the base URL at a repository that does not exist.

17 source-fence test files pass. Every new assertion in them was run against a deliberately
broken copy of the source first, because three fences in this branch turned out to be
satisfiable by a neighbouring match and would have passed with the bug present.

A seven-lens review with an adversarial verifier per finding raised 31 issues; the three that
survived were real, and are fixed in `bf1acbe547`.

## Not checked

**No hardware.** Everything above is a simulator. Per-game presets, the downloader and the
three review fixes have never run on a phone.

**The OpenGL and Vulkan backends were never compiled.** `GSDevice.h` gains a virtual here, and
`GSDeviceOGL.h` and `GSDeviceVK.h` gain overrides, but the iOS build forces both backends off,
so those two files have not been near a compiler. That is the most likely thing in this diff to
break CI, and it affects Windows, Linux, desktop macOS and Android rather than iOS.

**Search in the shader catalogue.** 867 rows in 27 sections and the field would not accept
synthetic keystrokes in the test harness, so the one control most people will reach for is the
one nobody has used.

Delete this file before merging.

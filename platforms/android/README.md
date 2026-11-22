# ARMSX2 
PlayStation 2 Emulator for Android based on the work of [PCSX2](https://github.com/PCSX2/pcsx2)

## App version

The Android app version is configured in `app/build.gradle.kts` under
`android.defaultConfig`:

```kotlin
versionCode = providers.gradleProperty("armsx2.versionCode").orNull?.toInt() ?: 1088
versionName = providers.gradleProperty("armsx2.versionName").orNull ?: "2.5.8"
```

Change `versionName` to the user-facing release version and increment the integer
`versionCode` for every published APK or AAB. Release scripts can still override
these defaults with `-Parmsx2.versionName=...` and `-Parmsx2.versionCode=...`.

## Android APK builds

A plain gradle build makes an APK for both 4K-page and 16K-page devices. The
core reads the host page size when it loads, and its ELF segments are 16K
aligned:

```bash
./gradlew :app:assembleGithubDebug
```

Releases are built with `tools/build-release-apk.sh` (sideload, through
`tools/build-release-targets.sh`) and `tools/build-play-aab.sh` (Play).

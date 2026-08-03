@file:Suppress("UnstableApiUsage", "DEPRECATION")
import org.gradle.api.GradleException
import java.util.Properties

plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.compose.compiler)
}

val armsx2NativeLibName = providers.gradleProperty("armsx2.nativeLibName").orElse("emucore_4k")
val armsx2Pgo = providers.gradleProperty("armsx2.pgo").orElse("none") // none | generate | optimize
val armsx2PgoProfile = providers.gradleProperty("armsx2.pgoProfile").orElse("") // abs path to merged .profdata (optimize)
val armsx2HostPageSize = providers.gradleProperty("armsx2.hostPageSize").orElse("0x1000")
// DIAGNOSTIC ONLY (-Parmsx2.recTestHooks=true): compiles the EERecFallback opcode-group
// interpreter bisect into the EE recompiler. Never set for a shipped build.
val armsx2RecTestHooks = providers.gradleProperty("armsx2.recTestHooks").orElse("false")
val armsx2ApplicationId = providers.gradleProperty("armsx2.applicationId").orElse("com.armsx2")
val armsx2SigningPropertiesFile = rootProject.file("armsx2_keystore.properties")
val armsx2SigningProperties = Properties().apply {
    if (armsx2SigningPropertiesFile.isFile) {
        armsx2SigningPropertiesFile.inputStream().use(::load)
    }
}
fun armsx2SigningProperty(name: String): String? = armsx2SigningProperties.getProperty(name)?.takeIf { it.isNotBlank() }
val armsx2PlaySigningReady = listOf("storeFile", "storePassword", "keyAlias", "keyPassword")
    .all { armsx2SigningProperty(it) != null }

if (armsx2SigningPropertiesFile.isFile && !armsx2PlaySigningReady) {
    throw GradleException("armsx2_keystore.properties is missing one or more required signing keys.")
}

// Runtime resources (GameDB, shaders, fonts, icons) have a single canonical copy
// at <repo>/bin/resources — the same tree the desktop/Linux build ships via
// pcsx2_copy_runtime_resources. Rather than commit a third drifting copy under
// src/main/assets, generate the APK's assets/resources from it at build time:
// sync bin/resources (minus the Windows-only dx11 shaders the mobile backends
// never compile) plus the canonical mobile GameDB overlay into a generated
// assets root. Genuine Android-only extras (patches.zip, the Noto color emoji
// font) stay committed under src/main/assets/resources and AGP merges the roots.
val generateSharedResources by tasks.registering(Sync::class) {
    val repoRoot = rootProject.layout.projectDirectory.dir("../..")
    from(repoRoot.dir("bin/resources")) {
        // Windows-only DX11 shaders the mobile backends never compile. The path is
        // shaders/dx11/ (relative to bin/resources), so "dx11/**" alone never matched.
        exclude("**/dx11/**")
    }
    from(repoRoot.file("bin/resources-overlay/armsx2_overrides.yaml"))
    into(layout.buildDirectory.dir("generated/sharedResources/resources"))
}

// AGP 9.2.1 rejects a Provider in assets.srcDir (the sourceSets block below uses a concrete path),
// so wire the dependency bmd's Provider form would have carried implicitly. The generated assets
// dir is consumed by several AGP tasks — asset merge AND lint's model/analyze passes — so declare
// generateSharedResources as a dependency of every one, plus the preBuild anchor. Gradle 9's strict
// validation fails the build otherwise ("uses this output without declaring a dependency").
tasks.matching { t ->
    t.name == "preBuild" || t.name.endsWith("Assets") || t.name.contains("Lint")
}.configureEach {
    dependsOn(generateSharedResources)
}

// Private, optional Discord SDK location. Set by release CI; absent in every public clone.
// Deliberately not a product flavour: one release variant, and the only difference is whether this
// directory was there at build time.
val armsx2DiscordSdkDir: String? =
    (System.getenv("DISCORD_SDK_DIR") ?: providers.gradleProperty("DISCORD_SDK_DIR").orNull)
        ?.takeIf { File(it, "include/discordpp.h").isFile }

android {
    namespace = "com.armsx2"
    compileSdk = 37

    defaultConfig {
        applicationId = armsx2ApplicationId.get()
        minSdk = 26
        targetSdk = 37
        versionCode = providers.gradleProperty("armsx2.versionCode").orNull?.toInt() ?: 1088
        versionName = providers.gradleProperty("armsx2.versionName").orNull ?: "2.6.1"

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
        ndk {
            abiFilters.add("arm64-v8a")
        }
    }

    signingConfigs {
        create("playRelease") {
            armsx2SigningProperty("storeFile")?.let { storeFile = rootProject.file(it) }
            storePassword = armsx2SigningProperty("storePassword")
            keyAlias = armsx2SigningProperty("keyAlias")
            keyPassword = armsx2SigningProperty("keyPassword")
        }
    }

    // The SDK's .so comes from the private directory, never from the repository.
    sourceSets {
        getByName("main") {
            if (armsx2DiscordSdkDir != null) jniLibs.srcDir(armsx2DiscordSdkDir)
        }
    }

    packaging {
        jniLibs {
            // The Discord .aar ships this .so AND the private dir supplies an identical copy for CMake
            // to link against. Byte-identical (same file, extracted from the same .aar), so taking
            // either is correct — without this the merger fails on the duplicate.
            pickFirsts += "**/libdiscord_partner_sdk.so"
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            // Sign release with the debug keystore so it's installable on-device
            // without a separate signing config. NOT for distribution — the debug
            // keystore is well-known and not secure for Play Store uploads.
            // Replace with a real release signingConfig before publishing.
            signingConfig = if (armsx2PlaySigningReady) {
                signingConfigs.getByName("playRelease")
            } else {
                signingConfigs.getByName("debug")
            }
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
            if (true) externalNativeBuild {
                cmake {
                    arguments += "-DANDROID=true"
                    arguments += "-DANDROID_STL=c++_static"
                    // Passed explicitly rather than relying on the environment leaking through to
                    // the CMake invocation, so a build is reproducible from the Gradle property
                    // alone. Absent = Discord compiles out.
                    armsx2DiscordSdkDir?.let { arguments += "-DDISCORD_SDK_DIR=$it" }
                    arguments += "-DCMAKE_BUILD_TYPE=Release"
                    if (armsx2RecTestHooks.get() == "true")
                        arguments += "-DENABLE_RECOMPILER_TEST_HOOKS=ON"
                    // PGO (profile-guided optimization), opt-in via -Parmsx2.pgo:
                    //   generate -> instrumented build (writes .profraw on-device); LTO OFF
                    //              for a faster/cleaner instrument pass.
                    //   optimize -> consume the merged profile (-fprofile-use); LTO ON.
                    //   <unset>  -> normal release, LTO ON (unchanged).
                    val pgo = armsx2Pgo.get()
                    arguments += if (pgo == "generate") "-DLTO_PCSX2_CORE=OFF" else "-DLTO_PCSX2_CORE=ON"
                    arguments += "-DARMSX2_EMUCORE_LIBRARY_NAME=${armsx2NativeLibName.get()}"
                    arguments += "-DARMSX2_ANDROID_HOST_PAGE_SIZE=${armsx2HostPageSize.get()}"
                    arguments += "-DCMAKE_C_FLAGS=-O3 -g"
                    arguments += "-DCMAKE_CXX_FLAGS=-O3 -g"
                    if (pgo == "generate") arguments += "-DUSE_PGO_GENERATE=ON"
                    if (pgo == "optimize") {
                        arguments += "-DUSE_PGO_OPTIMIZE=ON"
                        val prof = armsx2PgoProfile.get()
                        if (prof.isNotBlank()) arguments += "-DARMSX2_PGO_PROFILE=$prof"
                    }
                }
            }
        }
        debug {
            // Keep PCSX2_DEBUG/VIXL_DEBUG defines (via CMAKE_BUILD_TYPE=Debug)
            // but compile at -O3 to match release's
            // codegen. -O0 was exposing a JIT-adjacent crash in MGS2 that
            // -O3 release didn't hit, which narrows the cause to stack/
            // uninitialised-local fragility rather than the debug defines.
            // -ffp-contract=off was previously kept for VU1 bit-exactness
            // but only affects C/C++ FP code, not JIT-emitted FMUL/FADD.
            // Removing it lets the compiler fuse a*b+c → FMADD in counters,
            // GS software renderer, SPU2 audio mixing, IPU, VIF unpack —
            // significant FP-heavy paths. JIT'd VU FMAC semantics are
            // unaffected because the recompiler emits explicit Fmul+Fadd.
            externalNativeBuild {
                cmake {
                    arguments += "-DANDROID=true"
                    arguments += "-DANDROID_STL=c++_static"
                    // Passed explicitly rather than relying on the environment leaking through to
                    // the CMake invocation, so a build is reproducible from the Gradle property
                    // alone. Absent = Discord compiles out.
                    armsx2DiscordSdkDir?.let { arguments += "-DDISCORD_SDK_DIR=$it" }
                    arguments += "-DCMAKE_BUILD_TYPE=Debug"
                    arguments += "-DARMSX2_EMUCORE_LIBRARY_NAME=${armsx2NativeLibName.get()}"
                    arguments += "-DARMSX2_ANDROID_HOST_PAGE_SIZE=${armsx2HostPageSize.get()}"
                    arguments += "-DCMAKE_C_FLAGS=-O3 -g"
                    arguments += "-DCMAKE_CXX_FLAGS=-O3 -g"
                }
            }
        }
    }
    // Distribution split: the Play AAB (play flavor) stays scoped-storage /
    // SAF only — src/main/AndroidManifest.xml has NO MANAGE_EXTERNAL_STORAGE,
    // so play is Play-policy clean by construction. The sideloaded GitHub APK
    // (github flavor) merges src/github/AndroidManifest.xml, which adds
    // MANAGE_EXTERNAL_STORAGE back, and STORAGE_ALL_FILES gates the runtime
    // all-files / custom-folder path in the setup wizard. applicationId is left
    // to defaultConfig (driven by -Parmsx2.applicationId) so both flavors honor
    // the release/AAB pipeline's CLI override.
    flavorDimensions += "store"
    productFlavors {
        create("github") {
            dimension = "store"
            buildConfigField("boolean", "STORAGE_ALL_FILES", "true")
            // In-app GitHub-release updater. Github flavor only — Play forbids self-updating,
            // so the real updater + REQUEST_INSTALL_PACKAGES live in src/github and this stays
            // false for play (which uses the src/play no-op stub).
            buildConfigField("boolean", "IN_APP_UPDATER", "true")
        }
        create("play") {
            dimension = "store"
            buildConfigField("boolean", "STORAGE_ALL_FILES", "false")
            buildConfigField("boolean", "IN_APP_UPDATER", "false")
        }
    }
    // Merge the generated bin/resources tree in as a second assets root. Passing
    // the task's output provider (not a bare path) makes AGP's asset-merge tasks
    // depend on generateSharedResources, so the tree is materialized before it is
    // packaged. destinationDir is .../resources; its parent is the assets root.
    sourceSets.named("main") {
        // Concrete File — AGP 9.2.1's SourceSet API rejects Provider instances (bmd's
        // `generateSharedResources.map { }` form throws "cannot add Provider instances"). The
        // task dependency that materialises this tree is wired via tasks.matching above.
        assets.srcDir(layout.buildDirectory.dir("generated/sharedResources").get().asFile)
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }
    // NATIVE BUILD DISABLED for the recovered tree: the prebuilt native .so files
    // (extracted from vc1063 into src/main/jniLibs/arm64-v8a) are packaged directly,
    // so UI/Kotlin iteration doesn't require recompiling the C++ core. Re-enable this
    // block (and the per-buildType cmake blocks above) to rebuild native from source.
    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }
    buildFeatures {
        // Generated BuildConfig.DEBUG used by Main.kt's debug-only auto-boot
        // path. AGP 8 made this opt-in.
        buildConfig = true
    }

    packaging {
        jniLibs {
            // libadrenotools requires `useLegacyPackaging = true` so the
            // hook .so files (hook_impl, main_hook, file_redirect_hook,
            // gsl_alloc_hook) get extracted to ApplicationInfo.nativeLibraryDir
            // at install time. Without this, AGP leaves them inside the apk
            // and adrenotools' linker-namespace bypass can't find them by
            // path — the custom Vulkan driver load silently falls back to
            // the system loader.
            useLegacyPackaging = true
        }
    }
}

composeCompiler {
    // Keep R8 enabled while avoiding AGP's incompatible built-in Kotlin
    // compose-group-mapping producer. Source/line mappings remain preserved.
    includeComposeMappingFile.set(false)
}

// Android Studio's "Build > Clean Project" runs the `clean` task, but AGP
// leaves `app/.cxx/` (the CMake/Ninja workspace) in place. Stale .cxx state
// can lead to ghost builds — old object files linking against newer headers,
// or vice versa. Wire `cleanCxx` into `clean` so the native build workspace
// gets wiped too.
tasks.register<Delete>("cleanCxx") {
    delete(layout.projectDirectory.dir(".cxx"))
}
tasks.named("clean") {
    dependsOn("cleanCxx")
}

// Fail the build on any window-based modal in the Compose UI.
//
// A Compose Dialog / AlertDialog / DropdownMenu / ModalBottomSheet is its own FOCUSED ANDROID
// WINDOW. It consumes gamepad KeyEvents before they reach MainActivity.dispatchKeyEvent, which is
// where every D-pad route in this app lives — so anything built on one is unreachable with a
// controller no matter what is inside it. It fails silently and only on hardware the author
// probably does not have: flawless on a touchscreen, completely dead on a handheld.
//
// That is not hypothetical. It is the 2.6.0 "can't remap buttons" bug, and by the time this check
// was written it had been reintroduced at two dozen separate call sites, several of which had
// carefully registered controller ids INSIDE the dialog window — code that looks like it works and
// cannot. Use PadModal instead (ui/common/PadModal.kt), which is authored at the call site and
// drawn by one host above every surface.
//
// A check rather than a test because CI runs an assemble and never runs tests, so a unit test here
// would need a workflow change and would still be skippable locally.
//
// Escape hatch, if a genuine exception ever turns up: add the file's path to `allowed` below, with
// a comment saying why the pad does not need to reach it.
val checkNoWindowModals by tasks.registering {
    val sources = layout.projectDirectory.dir("src/main/java/com/armsx2")
    inputs.dir(sources)
    outputs.upToDateWhen { true }
    doLast {
        val banned = Regex("""\b(AlertDialog|ModalBottomSheet|DropdownMenu)\s*\(|\bandroidx\.compose\.(material3|ui\.window)\.(AlertDialog|ModalBottomSheet|DropdownMenu|Dialog)\b""")
        val allowed = setOf<String>(
            // (empty — every modal in the app goes through PadModal)
        )
        val hits = mutableListOf<String>()
        sources.asFile.walkTopDown().filter { it.isFile && it.extension == "kt" }.forEach { f ->
            val rel = f.relativeTo(sources.asFile).path
            if (rel in allowed) return@forEach
            f.readLines().forEachIndexed { i, line ->
                val code = line.substringBefore("//")
                if (banned.containsMatchIn(code)) hits += "$rel:${i + 1}: ${line.trim()}"
            }
        }
        if (hits.isNotEmpty()) {
            throw GradleException(
                buildString {
                    appendLine("Window-based modal found — a controller cannot reach it.")
                    appendLine()
                    appendLine("A Compose dialog/menu/sheet is its own focused Android window, so it eats gamepad")
                    appendLine("keys before MainActivity.dispatchKeyEvent — where all D-pad navigation lives. It")
                    appendLine("will look perfect on a touchscreen and be completely dead on a handheld.")
                    appendLine()
                    appendLine("Use PadModal / ConfirmOverlay / NotifyOverlay (ui/common/PadModal.kt) instead.")
                    appendLine()
                    hits.forEach { appendLine("  $it") }
                },
            )
        }
    }
}
tasks.named("preBuild") { dependsOn(checkNoWindowModals) }

dependencies {
    // Discord Social SDK, staged by hand rather than consumed as an .aar. The .aar's manifest
    // declares RECORD_AUDIO plus four foreground-service permissions and Bluetooth, all for its
    // voice features, and the manifest merger would fold every one of them into ARMSX2 -- the Play
    // listing would then show "Microphone" and need a data-safety declaration for a feature we do
    // not ship. Taking the pieces we want means we inherit no permissions at all: the native lib
    // lives in jniLibs, the headers under cpp/3rdparty/discord, and AuthenticationActivity is
    // declared in our own manifest. libwebrtc is here because the SDK's audio classes reference it
    // and would otherwise NoClassDefFoundError if any init path touches them.
    // The .aar, not its unpacked pieces: this is what delivers the SDK's manifest entries, its
    // consumer proguard rules and its transitive dependencies. Reconstructing those by hand cost
    // three separate bugs (boot crash, missing <queries>, missing androidx.browser).
    // Only when a private SDK directory was supplied. A public clone has none, so the whole
    // feature compiles out rather than failing to resolve — see DISCORD_SDK_DIR in cpp/CMakeLists.
    if (armsx2DiscordSdkDir != null) {
        implementation(group = "", name = "discord_partner_sdk", ext = "aar")
    }
    // REQUIRED by the Social SDK's authorization flow — its AuthenticationActivity drives sign-in
    // through Custom Tabs, and without these classes the OAuth round trip completes and the result
    // is then dropped on the Java side, with no error anywhere. Discord's Android guide lists it as
    // a dependency; we never got it because hand-staging the .aar bypasses the mechanism that
    // delivers a library's transitive dependencies. Third time that has bitten (proguard keeps,
    // <queries>, now this) — assume anything an .aar would have brought is missing until checked.
    implementation(libs.androidx.browser)

    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.appcompat)
    implementation(libs.material)

    //AndroidX Compose
    implementation(libs.androidx.activity.compose)
    implementation(platform(libs.androidx.compose.bom))
    implementation(libs.androidx.material3)
    implementation(libs.composeIcons.fontAwesome)
    implementation(libs.composeIcons.lineAwesome)

    implementation(libs.kotlin.reflect)
    implementation(libs.androidx.compose.ui.tooling.preview)
    implementation(libs.androidx.compose.foundation)
    implementation(libs.androidx.documentfile)
    implementation(libs.coil.compose)
    implementation(libs.coil.gif) // animated GIF / WebP / APNG (library background)
    implementation(libs.androidx.lifecycle.viewmodel.compose)
    implementation(libs.androidx.lifecycle.runtime.compose)

    testImplementation(libs.junit)
    androidTestImplementation(libs.androidx.junit)
    androidTestImplementation(libs.androidx.espresso.core)
    debugImplementation(libs.androidx.compose.ui.tooling)
}

# Preserve useful crash traces and metadata used by Android/Compose tooling.
-keepattributes SourceFile,LineNumberTable,Signature,InnerClasses,EnclosingMethod
-keepattributes RuntimeVisibleAnnotations,RuntimeInvisibleAnnotations,AnnotationDefault

# JNI entry points are resolved from native code by their exact class and member
# names (FindClass / GetStaticMethodID on literal strings). R8 can't see those
# native references, so every class the native side looks up by name must be kept
# un-renamed — this whole package is the JNI bridge layer (NativeApp, HttpClient +
# its nested Response). Keeping only NativeApp let R8 rename HttpClient, which broke
# the native HTTP downloader (RetroAchievements login / badge fetch → hard crash).
-keep class kr.co.iefriends.pcsx2.** { *; }
-keep class com.armsx2.BiosInfo { *; }
-keepclasseswithmembernames,includedescriptorclasses class * {
    native <methods>;
}

# Application components instantiated by the Android framework.
-keep class com.armsx2.Pasx2Application { *; }
-keep class com.armsx2.BootSplashActivity { *; }
-keep class com.armsx2.Main { *; }
-keep class com.armsx2.RetroAchievementsHostOverrideReceiver { *; }

# SDL resolves its Java bridge classes and callbacks through JNI/reflection.
-keep class org.libsdl.app.** { *; }

# ReLinker is reached reflectively by SDL on devices that need its fallback loader.
-keep class com.getkeepsafe.relinker.** { *; }
-dontwarn com.getkeepsafe.relinker.**

# Discord Social SDK. Its native JNI_OnLoad resolves these classes BY NAME, so R8 sees no Java
# reference to them, strips them, and the SDK aborts the process inside JNI_OnLoad with a
# ClassNotFoundException for AuthenticationClientCallback — a hard native abort during
# DiscordSocialSdkInit's static init, not a catchable Java exception.
#
# These rules are the vendor .aar's own consumer-proguard rules. We stage the SDK by hand rather
# than consuming the .aar (see build.gradle.kts for why: its manifest would merge RECORD_AUDIO and
# four foreground-service permissions into us), and doing that means its consumer rules never
# reach R8 — so they have to live here instead. Anything else carried over from an .aar by hand
# needs the same treatment.
-keep class com.discord.** { *; }
-keep class org.webrtc.** { *; }
-dontwarn com.discord.**
-dontwarn org.webrtc.**

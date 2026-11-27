# ENUMSX2 Patch 6

Patch 6 separates the device/profile rules from Android Build APIs so contributors can test them on the normal JVM test task.

Create these two files in GitHub:

1. `platforms/android/app/src/main/java/com/armsx2/enumsx2/Enumsx2MobileProfile.kt`
2. `platforms/android/app/src/test/java/com/armsx2/enumsx2/Enumsx2MobileProfileTest.kt`

For the Android Build wrapper from Patch 5, have it call:

`Enumsx2MobileProfile.matches(listOf(Build.MODEL, Build.DEVICE, Build.PRODUCT, Build.HARDWARE, Build.MANUFACTURER, if (Build.VERSION.SDK_INT >= 31) Build.SOC_MODEL else ""))`

and use:

`Enumsx2MobileProfile.apply(base)`

for the actual settings transformation.

This keeps matching and tuning testable without a physical phone.

Suggested commit:
`ENUMSX2 Patch 6: add testable mobile profile logic`

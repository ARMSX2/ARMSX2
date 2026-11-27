# ANNUMSX2 Android branding patch

Repository: ANNIX-AMMU/ANNUMSX2
Branch: enumsx2-a67l-blu-optimizations

## Replace this file

`platforms/android/app/src/main/res/values/strings.xml`

with the included `strings.xml`.

This changes the Android launcher/app label to ANNUMSX2 and the nightly label to ANNUMSX2 Nightly.

## Deliberately unchanged for now

- applicationId remains `com.armsx2`
- Kotlin/Java package paths remain `com.armsx2`
- native namespaces remain unchanged

Changing those identifiers is a much larger migration and is unnecessary just to brand the installed app. Keeping them stable reduces compile/regression risk while the fork is under active development.

Suggested commit:

`ANNUMSX2: update Android app branding`

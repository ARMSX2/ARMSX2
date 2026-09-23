package com.armsx2.config

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Settings and the per-section classes it holds (CpuSettings, GraphicsSettings, ...) are data
 * classes with one constructor parameter per field, and Kotlin gives each two synthetic methods
 * that take every one of those parameters plus some bookkeeping (shown here for Settings):
 *
 *  - copy$default(Settings, <every field>, <one Int bitmask per 32 fields>, Object), which every
 *    `settings.copy(x = ...)` call goes through;
 *  - <init>(<every field>, <one Int bitmask per 32 fields>, DefaultConstructorMarker), which every
 *    `Settings(...)` call that leaves any field at its default goes through (plus `this`).
 *
 * A dex call instruction can pass at most 255 argument registers, and a Long or Double takes
 * two. Past that, D8 does not fail the build: the register count wraps around in the encoded
 * instruction, and the device's verifier then rejects every class that makes such a call. In
 * debug builds that took out MainActivityRuntime and the app died at launch with
 * "VerifyError ... expected 0 argument registers, method signature has 1 or more" (256 wrapped
 * to 0), when Settings was still one flat class of 246 fields. Release builds happened to
 * survive because R8 strips the unused parameters.
 *
 * The JVM has the same 255-slot limit (counting `this`), so on the JVM an oversized class does
 * not even load: that shows up here as a ClassFormatError, reported with this explanation.
 *
 * When this fails for one of the section classes, split that section in two.
 */
class SettingsSizeTest {
    private val maxArgumentRegisters = 255

    private fun load(name: String): Class<*> =
        try {
            Class.forName(name)
        } catch (e: ClassFormatError) {
            throw AssertionError("$name has too many constructor parameters to load; see SettingsSizeTest", e)
        }

    /** Settings and every settings class nested inside it, found through constructor parameters. */
    private fun settingsClasses(): List<Class<*>> {
        val found = linkedSetOf<Class<*>>()
        fun visit(c: Class<*>) {
            if (!found.add(c)) return
            c.declaredConstructors.flatMap { it.parameterTypes.toList() }
                .filter { it.name.startsWith("com.armsx2.config.") && it.simpleName.endsWith("Settings") }
                .forEach { visit(load(it.name)) }
        }
        visit(load("com.armsx2.config.Settings"))
        return found.toList()
    }

    private fun registers(params: Array<Class<*>>): Int =
        params.fold(0) { n, t -> n + if (t == java.lang.Long.TYPE || t == java.lang.Double.TYPE) 2 else 1 }

    @Test
    fun findsTheNestedSettingsClasses() {
        val names = settingsClasses().map { it.simpleName }
        assertTrue(names.toString(), "GraphicsSettings" in names && "AchievementsSettings" in names)
    }

    @Test
    fun copyDefaultFitsInOneDexCall() {
        for (c in settingsClasses()) {
            val copyDefault = c.declaredMethods.single { it.name == "copy\$default" }
            // Static, so the receiver is already its first explicit parameter.
            val used = registers(copyDefault.parameterTypes)
            assertTrue(
                "${c.simpleName}.copy\$default needs $used argument registers; a dex call allows $maxArgumentRegisters",
                used <= maxArgumentRegisters,
            )
        }
    }

    @Test
    fun defaultingConstructorFitsInOneDexCall() {
        for (c in settingsClasses()) {
            val ctor = c.declaredConstructors.single {
                it.parameterTypes.lastOrNull()?.name == "kotlin.jvm.internal.DefaultConstructorMarker"
            }
            val used = 1 + registers(ctor.parameterTypes) // + `this`
            assertTrue(
                "${c.simpleName}'s defaulting constructor needs $used argument registers; a dex call allows $maxArgumentRegisters",
                used <= maxArgumentRegisters,
            )
        }
    }

    /** Settings are stored nested, but a game's own values must still be saved and read back
     *  under the flat keys older installs already have on disk. */
    @Test
    fun achievementsOverridesSurviveDiffAndMerge() {
        val global = Settings()
        val achievements = global.emuCore.achievements.copy(hardcore = true, notificationScale = 150)
        val game = global.copy(emuCore = global.emuCore.copy(achievements = achievements))

        val overrides = Settings.diff(global, game)
        assertEquals(setOf("achievementsHardcore", "achievementsNotificationScale"), overrides.keys().asSequence().toSet())
        assertEquals(game, Settings.merge(global, overrides))
        assertEquals(game, Settings.fromJson(game.toJson()))
    }
}

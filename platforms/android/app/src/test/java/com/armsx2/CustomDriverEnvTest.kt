package com.armsx2

import org.junit.Assert.assertEquals
import org.junit.Test

class CustomDriverEnvTest {
    @Test
    fun keepsMesaVariablesOnly() {
        val vars = CustomDriver.parseDriverEnv(
            """
            # comment
            TU_DEBUG=sysmem,nolrz
            FD_DEV_FEATURES = enable_tp_ubwc_flag_hint=1
            LIBVULKAN_PATH=/sdcard/x.so
            ARMSX2_ANGLE_EGL_LIBRARY=/x
            MESA_SHADER_CACHE_DISABLE=true
            IR3_SHADER_DEBUG=nouboopt
            lowercase_name=1
            =novalue
            not a line
            """.trimIndent()
        )
        assertEquals(
            linkedMapOf(
                "TU_DEBUG" to "sysmem,nolrz",
                "FD_DEV_FEATURES" to "enable_tp_ubwc_flag_hint=1",
                "MESA_SHADER_CACHE_DISABLE" to "true",
                "IR3_SHADER_DEBUG" to "nouboopt",
            ),
            vars,
        )
    }

    @Test
    fun ubwcHintAlone() {
        assertEquals(
            mapOf("FD_DEV_FEATURES" to "enable_tp_ubwc_flag_hint=1"),
            CustomDriver.driverEnv("", ubwcHint = true),
        )
    }

    @Test
    fun ubwcHintJoinsExistingFeatures() {
        assertEquals(
            "has_lrz_feedback=0:enable_tp_ubwc_flag_hint=1",
            CustomDriver.driverEnv("FD_DEV_FEATURES=has_lrz_feedback=0", ubwcHint = true)["FD_DEV_FEATURES"],
        )
    }

    @Test
    fun ubwcHintNotDoubled() {
        assertEquals(
            "enable_tp_ubwc_flag_hint=0",
            CustomDriver.driverEnv("FD_DEV_FEATURES=enable_tp_ubwc_flag_hint=0", ubwcHint = true)["FD_DEV_FEATURES"],
        )
    }

    @Test
    fun nothingWhenOff() {
        assertEquals(emptyMap<String, String>(), CustomDriver.driverEnv("", ubwcHint = false))
    }
}

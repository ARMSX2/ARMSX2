package com.armsx2.ui.saves

import kr.co.iefriends.pcsx2.NativeApp

/**
 * [message] with the core's reason for the last refused state load under it ("Memory size mismatch,
 * ..."), when the core gave one. Call it right after the load that failed.
 */
fun withStateLoadReason(message: String): String {
    val reason = runCatching { NativeApp.getLastStateLoadError() }.getOrNull().orEmpty().trim()
    return if (reason.isEmpty()) message else "$message\n$reason"
}

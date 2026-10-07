package com.armsx2.ui

import com.armsx2.EmuState

/** Only bare gameplay owns Back; visible frontend surfaces keep their own handlers. */
internal fun handlesGameplayBack(state: EmuState, frontendCovers: Boolean): Boolean =
    (state == EmuState.RUNNING || state == EmuState.PAUSED) && !frontendCovers

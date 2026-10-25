package com.armsx2.ui

import com.armsx2.ui.emulation.EmulationMenuTab

/**
 * Where the in-game menu was left, so it reopens there: on the tab last shown and, when the pad was in that tab's
 * pane, on the control it had highlighted. Each tab's scroll is kept as well (InGameOverlay.menuTabScroll). A
 * shortcut that opens a given tab (Options, Achievements) still opens that tab.
 *
 * For this run of the app only: nothing is saved, and it never reaches the core.
 *
 * Requested because checking achievements or trying rendering options over and over meant walking back to the
 * same place in the menu every time it opened.
 */
object MenuPlace {
    // The tab, whether the pad was in its pane, and on what.
    var tab: EmulationMenuTab? = null
        private set
    var inContent = false
        private set
    var controlId: String? = null
        private set

    /** The menu's place as the pad or a tap last left it. */
    fun note(tab: EmulationMenuTab, inContent: Boolean, controlId: String?) {
        this.tab = tab
        this.inContent = inContent && controlId != null
        this.controlId = if (this.inContent) controlId else null
    }
}

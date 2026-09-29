package com.armsx2.ui.home

import com.armsx2.memcard.MemcardCovers
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/** The Icon Museum's Shuffle: the icon showing in the middle, random others either side. */
class ShuffleWindowTest {
    private fun icons(n: Int) = List(n) { i -> MemcardCovers.ShowIcon("k$i", "Icon $i", null, MemcardCovers.FromDisc) { null } }

    @Test
    fun theIconShowingStaysInTheMiddleWithRandomOthersAround() {
        val all = icons(4000)
        repeat(200) {
            val here = all.random()
            val w = shuffleAround(all, here)
            assertEquals(here.key, w.pages[w.middle].key)
            assertEquals(SHUFFLE_REACH, w.middle)
            assertEquals(2 * SHUFFLE_REACH + 1, w.pages.size)
            assertEquals(w.pages.size, w.pages.map { it.key }.toSet().size)
            assertFalse(w.pages.withIndex().any { (i, s) -> i != w.middle && s.key == here.key })
        }
    }

    @Test
    fun stepsFromTheSameIconLeadSomewhereNewEachTime() {
        val all = icons(4000)
        val here = all[17]
        val rights = List(50) { shuffleAround(all, here).let { it.pages[it.middle + 1].key } }
        assertTrue(rights.toSet().size > 40)
    }

    @Test
    fun smallListsStillWork() {
        assertEquals(listOf("k0"), shuffleAround(icons(1), icons(1)[0]).pages.map { it.key })
        val two = icons(2)
        val w = shuffleAround(two, two[0])
        assertEquals(listOf("k0", "k1"), w.pages.map { it.key })
        assertEquals(0, w.middle)
        val five = icons(5)
        val w5 = shuffleAround(five, five[2])
        assertEquals(5, w5.pages.size)
        assertEquals("k2", w5.pages[w5.middle].key)
        assertEquals(five.map { it.key }.toSet(), w5.pages.map { it.key }.toSet())
    }
}

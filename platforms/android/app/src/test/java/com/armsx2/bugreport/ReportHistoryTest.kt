package com.armsx2.bugreport

import com.armsx2.config.FakeSharedPreferences
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/** "My reports": newest first, one line per report, and still there after the folder is gone. */
class ReportHistoryTest {
    private fun entry(id: String, at: Long, status: HistoryStatus = HistoryStatus.UPLOADING, code: String = "") =
        HistoryEntry(id, code, "graphics", at, "Game $id", "SLUS-1", status, 1234)

    @Test
    fun listsNewestFirstAndUpdatesInPlace() {
        val history = ReportHistory(FakeSharedPreferences())
        history.upsert(entry("a", 100))
        history.upsert(entry("b", 300))
        history.upsert(entry("c", 200))
        assertEquals(listOf("b", "c", "a"), history.list().map { it.localId })

        history.upsert(entry("c", 200, HistoryStatus.SENT, code = "7KQ4M2PD"))
        val c = history.find("c")!!
        assertEquals(HistoryStatus.SENT, c.status)
        assertEquals("7KQ4M2PD", c.code)
        assertEquals(3, history.list().size)
    }

    @Test
    fun survivesBeingReadByAnotherInstance() {
        val prefs = FakeSharedPreferences()
        ReportHistory(prefs).upsert(entry("a", 100, HistoryStatus.WAITING_RETRY))
        val back = ReportHistory(prefs).find("a")!!
        assertEquals(HistoryStatus.WAITING_RETRY, back.status)
        assertEquals("Game a", back.title)
        assertEquals(1234L, back.bytes)
    }

    @Test
    fun removeDropsOnlyThatLine() {
        val history = ReportHistory(FakeSharedPreferences())
        history.upsert(entry("a", 1))
        history.upsert(entry("b", 2))
        history.remove("a")
        assertNull(history.find("a"))
        assertEquals(listOf("b"), history.list().map { it.localId })
    }

    @Test
    fun keepsOnlyTheMostRecentFifty() {
        val history = ReportHistory(FakeSharedPreferences())
        for (i in 1..60) history.upsert(entry("r$i", i.toLong()))
        val all = history.list()
        assertEquals(ReportHistory.MAX_ENTRIES, all.size)
        assertEquals("r60", all.first().localId)
        assertEquals("r11", all.last().localId)
    }

    @Test
    fun anUnreadableStoreIsEmptyNotACrash() {
        val prefs = FakeSharedPreferences()
        prefs.edit().putString("history", "{broken").commit()
        assertEquals(emptyList<HistoryEntry>(), ReportHistory(prefs).list())
    }
}

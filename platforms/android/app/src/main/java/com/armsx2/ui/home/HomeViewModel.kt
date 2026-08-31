package com.armsx2.ui.home

import android.app.Application
import androidx.lifecycle.AndroidViewModel
import com.armsx2.GameInfo
import com.armsx2.data.library.GameLibraryRepository
import com.armsx2.runtime.MainActivityRuntime
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch
import androidx.core.content.edit

enum class HomeSort { Title, RecentlyPlayed, Compatibility }

enum class LibraryLayout { Grid, List, Shelf }

data class HomeUiState(
    val allGames: List<GameInfo> = emptyList(),
    val visibleGames: List<GameInfo> = emptyList(),
    val recentGames: List<GameInfo> = emptyList(),
    val query: String = "",
    val sort: HomeSort = HomeSort.Title,
    val layout: LibraryLayout = LibraryLayout.Grid,
    /** Category the LIST layout is filtered to, or null for everything. Only List uses it —
     *  Grid and Shelf show categories as their own sections instead, the way Recently Played
     *  works, so filtering the whole library there would hide the sections it is made of. */
    val categoryFilter: String? = null,
    val scanning: Boolean = false,
    val error: String? = null,
    val selectedIndex: Int = 0,
    val initialized: Boolean = false,
)

class HomeViewModel(application: Application) : AndroidViewModel(application) {
    private val repository = GameLibraryRepository(application)
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private var scanJob: Job? = null
    private var loaded = false
    private var pendingInitialScan = false
    private var directories: List<String> = emptyList()

    var state = androidx.compose.runtime.mutableStateOf(HomeUiState())
        private set

    fun load(romDirectories: List<String>, nativeReady: Boolean) {
        directories = romDirectories
        if (!loaded) {
            loaded = true
            val cached = repository.loadCached()
            val layout = runCatching {
                LibraryLayout.valueOf(
                    MainActivityRuntime.prefs.getString(LayoutPreference, LibraryLayout.Grid.name) ?: LibraryLayout.Grid.name,
                )
            }.getOrDefault(LibraryLayout.Grid)
            pendingInitialScan = romDirectories.isNotEmpty() && cached.key != repository.cacheKey(romDirectories)
            state.value = buildState(
                base = state.value.copy(
                    allGames = cached.games,
                    layout = layout,
                    initialized = cached.games.isNotEmpty() || !pendingInitialScan,
                ),
            )
            // Library-wide RetroAchievements progress. Hooked here because this is where the game
            // list lives, and the sync needs paths to hash. No-op unless a web API key is set.
            if (nativeReady) com.armsx2.RaLibrary.onLibraryLoaded(state.value.allGames)
            if (nativeReady && pendingInitialScan) refresh()
        } else if (nativeReady && pendingInitialScan) {
            refresh()
        }
    }

    fun refresh() {
        if (directories.isEmpty() || scanJob?.isActive == true) return
        scanJob = scope.launch {
            val initialScan = pendingInitialScan && state.value.allGames.isEmpty()
            state.value = state.value.copy(
                scanning = true,
                initialized = if (initialScan) false else state.value.initialized,
                error = null,
            )
            val result = runCatching { repository.scan(directories) }
            result.onSuccess { games ->
                pendingInitialScan = false
                state.value = buildState(
                    state.value.copy(allGames = games, scanning = false, initialized = true),
                )
                com.armsx2.RaLibrary.onLibraryLoaded(games)
            }.onFailure { failure ->
                pendingInitialScan = false
                state.value = state.value.copy(
                    scanning = false,
                    initialized = true,
                    error = failure.message ?: "Unable to scan the selected folders.",
                )
            }
        }
    }

    fun setQuery(value: String) {
        state.value = buildState(state.value.copy(query = value, selectedIndex = 0))
    }

    fun setSort(value: HomeSort) {
        state.value = buildState(state.value.copy(sort = value, selectedIndex = 0))
    }

    fun toggleLayout() {
        val entries = LibraryLayout.entries
        val next = entries[(state.value.layout.ordinal + 1) % entries.size]
        MainActivityRuntime.prefs.edit { putString(LayoutPreference, next.name) }
        state.value = state.value.copy(layout = next)
    }

    fun moveSelection(delta: Int) {
        val count = state.value.visibleGames.size
        if (count == 0) return
        state.value = state.value.copy(selectedIndex = (state.value.selectedIndex + delta).coerceIn(0, count - 1))
    }

    fun setSelection(index: Int) {
        if (state.value.visibleGames.isEmpty()) return
        state.value = state.value.copy(selectedIndex = index.coerceIn(state.value.visibleGames.indices))
    }

    fun selectedGame(): GameInfo? = state.value.visibleGames.getOrNull(state.value.selectedIndex)

    /** List-layout category filter. Null shows everything. */
    fun setCategoryFilter(category: String?) {
        state.value = buildState(state.value.copy(categoryFilter = category, selectedIndex = 0))
    }

    fun launch(game: GameInfo) {
        repository.markPlayed(game)
        state.value = buildState(state.value)
        val launchPath = if (game.uri.scheme == "file") game.uri.path ?: game.uri.toString() else game.uri.toString()
        MainActivityRuntime.launchGame(launchPath, game)
    }

    /** Mark a game hidden (or un-hidden) and refresh the visible list. */
    fun setHidden(game: GameInfo, value: Boolean) {
        com.armsx2.HiddenGames.setHidden(game, value)
        state.value = buildState(state.value)
    }

    /** Toggle whether hidden games are shown (so they can be un-hidden). */
    fun setShowHidden(value: Boolean) {
        com.armsx2.HiddenGames.setShowHidden(value)
        state.value = buildState(state.value)
    }

    /** Drop one game from Recently Played (it returns when next launched). */
    fun removeFromRecent(game: GameInfo) {
        repository.removeFromRecent(game)
        state.value = buildState(state.value)
    }

    /** Empty Recently Played. Games return as they're launched again; the library is untouched. */
    fun clearRecent() {
        repository.clearRecent()
        state.value = buildState(state.value)
    }

    private fun buildState(base: HomeUiState): HomeUiState {
        val recents = repository.recentGames(base.allGames)
        val recentOrder = recents.mapIndexed { index, game -> game.uri.toString() to index }.toMap()
        val forceEn = com.armsx2.EnglishTitles.enabled.value
        // A category filter narrows the pool before anything else. Cleared implicitly when the
        // category stops existing (renamed or deleted while selected) rather than leaving the
        // library stuck showing nothing with no obvious way back.
        val activeCategory = base.categoryFilter?.takeIf { it in com.armsx2.GameCategories.names() }
        val members = activeCategory?.let { com.armsx2.GameCategories.members(it) }
        val filtered = base.allGames.filter { game ->
            val query = base.query.trim()
            (members == null || game.settingsKey?.let { it in members } == true) &&
            // Exclude games the user marked hidden (long-press → Hide), unless "Show hidden" is on.
            (com.armsx2.HiddenGames.showHidden.value || !com.armsx2.HiddenGames.isHidden(game)) &&
                (query.isBlank() ||
                    // Match BOTH names regardless of which is displayed: someone typing
                    // "Katakamuna" should find a game listed as 片神名, and vice versa.
                    game.title.contains(query, ignoreCase = true) ||
                    game.titleEn.contains(query, ignoreCase = true) ||
                    game.titleSort.contains(query, ignoreCase = true) ||
                    game.serial?.contains(query, ignoreCase = true) == true ||
                    game.extension.contains(query, ignoreCase = true))
        }
        // sortKey(), not the displayed title: a Japanese title sorts by its kana reading
        // (GameDB name-sort), because sorting the kanji sorts by codepoint — which is the
        // "sort by name-sort" half of issue #338.
        val sorted = when (base.sort) {
            HomeSort.Title -> filtered.sortedBy { it.sortKey(forceEn).lowercase() }
            HomeSort.RecentlyPlayed -> filtered.sortedWith(
                compareBy<GameInfo> { recentOrder[it.uri.toString()] ?: Int.MAX_VALUE }
                    .thenBy { it.sortKey(forceEn).lowercase() },
            )
            HomeSort.Compatibility -> filtered.sortedWith(
                compareByDescending<GameInfo> { it.compatibility }
                    .thenBy { it.sortKey(forceEn).lowercase() },
            )
        }
        return base.copy(
            visibleGames = sorted,
            recentGames = recents,
            selectedIndex = base.selectedIndex.coerceIn(0, (sorted.size - 1).coerceAtLeast(0)),
        )
    }

    override fun onCleared() {
        scope.cancel()
        super.onCleared()
    }

    private companion object {
        // String-valued now (Grid/List/Shelf). New key so the old boolean pref is
        // ignored and everyone starts at Grid rather than mis-parsing "true"/"false".
        const val LayoutPreference = "library.layout.mode"
    }
}

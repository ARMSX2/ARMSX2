package com.armsx2.ui.news

import android.app.Application
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.armsx2.News
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

data class NewsUiState(
    val items: List<News.Item> = emptyList(),
    val loading: Boolean = false,
    val fromCache: Boolean = false,
)

class NewsViewModel(application: Application) : AndroidViewModel(application) {
    var state = androidx.compose.runtime.mutableStateOf(NewsUiState())
        private set

    private var loaded = false

    /**
     * [force] skips the cache TTL — the refresh button. Without it, opening the screen twice in a
     * session does no network at all, which is the point of the cache.
     */
    fun load(force: Boolean = false) {
        if (loaded && !force) return
        loaded = true
        state.value = state.value.copy(loading = true)
        viewModelScope.launch {
            // News.fetch does network and file I/O and is documented as blocking.
            val result = withContext(Dispatchers.IO) {
                runCatching { News.fetch(getApplication(), force) }.getOrNull()
            }
            state.value = NewsUiState(
                items = result?.items.orEmpty(),
                loading = false,
                fromCache = result?.fromCache == true,
            )
        }
    }
}

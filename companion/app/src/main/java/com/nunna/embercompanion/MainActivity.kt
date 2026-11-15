package com.nunna.embercompanion

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.BackHandler
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import com.nunna.embercompanion.android.AppGraph
import com.nunna.embercompanion.android.LinkService
import com.nunna.embercompanion.ui.*

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        AppGraph.init(this)
        LinkService.start(this)
        setContent {
            val ember = Color(0xFFE4572E)
            val scheme = if (isSystemInDarkTheme()) darkColorScheme(primary = ember) else lightColorScheme(primary = ember)
            MaterialTheme(colorScheme = scheme) {
                Surface(Modifier.fillMaxSize()) {
                    var screen by rememberSaveable { mutableStateOf(Screen.HOME.name) }
                    var openNote by rememberSaveable { mutableStateOf<String?>(null) }
                    val cur = Screen.valueOf(screen)
                    BackHandler(enabled = cur != Screen.HOME) {
                        screen = when (cur) { Screen.NOTE -> Screen.NOTES.name; Screen.BENCH -> Screen.SETTINGS.name; else -> Screen.HOME.name }
                    }
                    ConfirmDialog()
                    when (cur) {
                        Screen.HOME -> HomeScreen { screen = it.name }
                        Screen.NOTIFS -> NotifScreen { screen = Screen.HOME.name }
                        Screen.STEPS -> StepsScreen { screen = Screen.HOME.name }
                        Screen.APPS -> WatchAppsScreen { screen = Screen.HOME.name }
                        Screen.NOTES -> NotesScreen(onBack = { screen = Screen.HOME.name }, onOpen = { openNote = it; screen = Screen.NOTE.name })
                        Screen.NOTE -> NoteScreen(openNote ?: "") { screen = Screen.NOTES.name }
                        Screen.TRANSLATE -> TranslateScreen { screen = Screen.HOME.name }
                        Screen.PRACTICE -> PracticeScreen { screen = Screen.HOME.name }
                        Screen.SETTINGS -> SettingsScreen(go = { screen = it.name }) { screen = Screen.HOME.name }
                        Screen.BENCH -> BenchScreen { screen = Screen.SETTINGS.name }
                    }
                }
            }
        }
    }
}

package com.nunna.embercompanion.protocol

/** The Lua apps bundled in assets/lua. The file name (without .lua) becomes the app name on the watch. */
data class LuaCatalogEntry(val name: String, val title: String, val blurb: String) {
    val assetPath get() = "lua/$name.lua"
}

object LuaCatalog {
    val entries = listOf(
        LuaCatalogEntry("pomodoro", "Pomodoro", "25 minutes of focus, then a 5 minute break."),
        LuaCatalogEntry("habit", "Habit tick", "Tick off three daily habits and keep a streak."),
        LuaCatalogEntry("water", "Water counter", "Count glasses of water. Goal is 8 a day."),
        LuaCatalogEntry("interval", "Interval timer", "30 seconds of work, 10 of rest, 8 rounds."),
        LuaCatalogEntry("breathe", "Box breathing", "Calm 4-4-4-4 breathing with a counter."),
    )
}

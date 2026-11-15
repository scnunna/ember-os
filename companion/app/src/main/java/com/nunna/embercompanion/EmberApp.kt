package com.nunna.embercompanion

import android.app.Activity
import android.app.Application
import android.os.Bundle
import android.widget.TextView
import com.nunna.embercompanion.android.AppGraph

class EmberApp : Application() {
    override fun onCreate() {
        super.onCreate()
        AppGraph.init(this)
    }
}

/** Health Connect opens this to explain what we do with steps. */
class HealthPrivacyActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val t = TextView(this)
        t.setPadding(48, 96, 48, 48)
        t.textSize = 16f
        t.text = "Ember Companion only writes the daily step counts that your watch reports. " +
            "It never reads your health data and never sends anything off your phone."
        setContentView(t)
    }
}

plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
    alias(libs.plugins.kotlin.compose)
    alias(libs.plugins.kotlin.serialization)
}

android {
    namespace = "com.nunna.embercompanion"
    compileSdk = 36

    defaultConfig {
        applicationId = "com.nunna.embercompanion"
        minSdk = 31
        targetSdk = 36
        versionCode = 1
        versionName = "0.1.0"
        // The bundled sherpa-onnx native libraries are arm64 only (the Pixel 8).
        ndk { abiFilters += "arm64-v8a" }
    }

    buildTypes {
        release { isMinifyEnabled = false }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    buildFeatures { compose = true }
    lint { checkReleaseBuilds = false; abortOnError = false }
    packaging { jniLibs { useLegacyPackaging = true } }
    testOptions {
        unitTests {
            isIncludeAndroidResources = true
            isReturnDefaultValues = true
            all { it.maxHeapSize = "2g" }
        }
    }
}

kotlin { jvmToolchain(17) }

dependencies {
    implementation(libs.kotlinx.coroutines.android)
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.activity.compose)
    implementation(platform(libs.androidx.compose.bom))
    implementation(libs.androidx.compose.ui)
    implementation(libs.androidx.compose.ui.tooling.preview)
    implementation(libs.androidx.compose.material3)
    implementation(libs.androidx.compose.icons)
    implementation(libs.androidx.navigation.compose)
    implementation(libs.kotlinx.serialization.json)
    implementation(libs.androidx.health.connect)
    implementation(project(":shared"))
    implementation(libs.litertlm)
    implementation(libs.commons.compress)
    debugImplementation(libs.androidx.compose.ui.tooling)
    testImplementation(project(":shared"))
    testImplementation(libs.junit)
    testImplementation(libs.kotlinx.coroutines.test)
    testImplementation(libs.robolectric)
    testImplementation(libs.androidx.test.core)
    testImplementation(libs.androidx.test.ext.junit)
}

// S5: the companion must not be able to use the internet. This fails the build if INTERNET shows up in the merged
// manifest (for example because a library added it).
val verifyNoInternet = tasks.register("verifyNoInternet") {
    dependsOn("processDebugMainManifest")
    val dir = layout.buildDirectory.dir("intermediates/merged_manifests/debug")
    doLast {
        val files = fileTree(dir).matching { include("**/AndroidManifest.xml") }.files
        check(files.isNotEmpty()) { "merged manifest not found" }
        for (f in files) {
            val t = f.readText()
            check(!t.contains("android.permission.INTERNET")) { "S5 violated: INTERNET is in the merged manifest ${f.path}" }
        }
    }
}
tasks.matching { it.name == "assembleDebug" || it.name == "testDebugUnitTest" }.configureEach { dependsOn(verifyNoInternet) }

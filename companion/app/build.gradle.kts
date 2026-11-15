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
    implementation(libs.mediapipe.genai)
    implementation(libs.mlkit.translate)
    implementation(libs.commons.compress)
    debugImplementation(libs.androidx.compose.ui.tooling)
    testImplementation(libs.junit)
    testImplementation(libs.kotlinx.coroutines.test)
    testImplementation(libs.robolectric)
    testImplementation(libs.androidx.test.core)
    testImplementation(libs.androidx.test.ext.junit)
}

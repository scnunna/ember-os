plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
    alias(libs.plugins.kotlin.compose)
}

android {
    namespace = "com.nunna.embergateway"
    compileSdk = 36

    defaultConfig {
        applicationId = "com.nunna.embergateway"
        minSdk = 31
        targetSdk = 36
        versionCode = 1
        versionName = "0.1.0"
    }
    buildTypes { release { isMinifyEnabled = false } }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    buildFeatures { compose = true }
    lint { checkReleaseBuilds = false; abortOnError = false }
}

kotlin { jvmToolchain(17) }

dependencies {
    implementation(project(":shared"))
    implementation(libs.kotlinx.coroutines.android)
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.activity.compose)
    implementation(platform(libs.androidx.compose.bom))
    implementation(libs.androidx.compose.ui)
    implementation(libs.androidx.compose.material3)
    implementation(libs.kotlinx.serialization.json)
}

// Proof for the report: the gateway must be the app that holds INTERNET.
tasks.register("verifyHasInternet") {
    dependsOn("processDebugMainManifest")
    doLast {
        val f = fileTree(layout.buildDirectory.dir("intermediates/merged_manifests/debug")).matching { include("**/AndroidManifest.xml") }.files.first()
        check(f.readText().contains("android.permission.INTERNET")) { "Gateway manifest lost INTERNET" }
    }
}

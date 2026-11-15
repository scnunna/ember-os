plugins {
    alias(libs.plugins.kotlin.jvm)
    alias(libs.plugins.kotlin.serialization)
}

kotlin { jvmToolchain(17) }

dependencies {
    implementation(libs.kotlinx.serialization.json)
    testImplementation(libs.junit)
}

// So that `gradlew testDebugUnitTest` also runs these JVM tests.
tasks.register("testDebugUnitTest") { dependsOn("test") }

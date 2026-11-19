plugins {
    id("com.android.application")
}

android {
    namespace = "com.castairplay.usbprobe"
    compileSdk = 36

    defaultConfig {
        applicationId = "com.castairplay.usbprobe"
        minSdk = 23
        targetSdk = 35
        versionCode = 1
        versionName = "1.0"
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

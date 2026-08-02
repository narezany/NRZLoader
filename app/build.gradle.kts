// Версия приложения берётся оттуда же, откуда её берут загрузчик и лаунчер.
// Раньше в свойствах пакета висела 1.0, пока всё остальное ушло вперёд.
val loaderVersion = file("../VERSION").readText().trim()

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.plugin.compose")
}

android {
    namespace = "ru.narezany.nrzloader"
    compileSdk = 34

    defaultConfig {
        applicationId = "ru.narezany.nrzloader"
        minSdk = 26
        targetSdk = 34
        // Каждая часть версии даёт свой разряд: 1.1.1 -> 10101.
        versionCode = loaderVersion.split(".").let {
            (it.getOrNull(0)?.toIntOrNull() ?: 0) * 10000 +
                (it.getOrNull(1)?.toIntOrNull() ?: 0) * 100 +
                (it.getOrNull(2)?.toIntOrNull() ?: 0)
        }
        versionName = loaderVersion
    }

    signingConfigs {
        create("shipping") {
            // The same key the patcher signs with. A modded package only needs
            // a consistent signature so updates install over each other.
            storeFile = file("src/main/assets/nrzloader.p12")
            storePassword = "nrzloader"
            keyAlias = "nrzloader"
            keyPassword = "nrzloader"
            storeType = "PKCS12"
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            signingConfig = signingConfigs.getByName("shipping")
        }
        debug {
            signingConfig = signingConfigs.getByName("shipping")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }

    buildFeatures {
        compose = true
        buildConfig = true
    }

    packaging {
        // The loader ships as an asset rather than a library of this app: it
        // belongs to the patched game, not here.
        resources.excludes += "/META-INF/{AL2.0,LGPL2.1}"
    }

    // The engine's tests run on a desktop JVM and are driven by a script.
    sourceSets["test"].java.srcDirs("src/test/java")
}

tasks.withType<Test> {
    // Проверка на настоящей библиотеке включается свойством, чтобы обычный
    // прогон не зависел от того, лежит ли она на этой машине.
    System.getProperty("nrz.so")?.let { systemProperty("nrz.so", it) }
}

dependencies {
    implementation(files("libs/apksig.jar"))

    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.activity:activity-compose:1.9.3")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.8.7")
    implementation("androidx.lifecycle:lifecycle-runtime-compose:2.8.7")
    implementation("androidx.documentfile:documentfile:1.0.1")

    val composeBom = platform("androidx.compose:compose-bom:2024.10.01")
    implementation(composeBom)
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.material3:material3")
    implementation("androidx.compose.material:material-icons-extended")
    implementation("androidx.compose.ui:ui-tooling-preview")

    // Разбор библиотеки игры от Android не зависит и проверяется на обычной
    // JVM: на телефоне эталона нет, а от правильности разбора зависит вывод.
    testImplementation("junit:junit:4.13.2")
}

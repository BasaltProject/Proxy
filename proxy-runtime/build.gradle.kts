import org.jetbrains.kotlin.gradle.plugin.mpp.KotlinNativeTarget

plugins {
    id("basalt.native-module")
}

kotlin {
    sourceSets {
        commonMain.dependencies {
            api(project(":wire-common"))
            api(project(":wire-io"))
            api(project(":wire-protocol"))
            api(project(":wire-text"))
            api(project(":wire-auth"))
        }
    }

    targets.withType<KotlinNativeTarget> {
        binaries {
            executable {
                entryPoint = "org.basaltmc.proxy.runtime.main"
                // Pinned, so the module can be renamed without renaming what operators download.
                baseName = "basalt-proxy"
            }
        }
    }
}

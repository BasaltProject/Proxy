import org.jetbrains.kotlin.gradle.plugin.mpp.KotlinNativeTarget

plugins {
    id("basalt.native-module")
}

kotlin {
    sourceSets {
        commonMain.dependencies {
            api(project(":basalt-common"))
            api(project(":basalt-io"))
            api(project(":basalt-protocol"))
            api(project(":basalt-text"))
            api(project(":basalt-auth"))
        }
    }

    targets.withType<KotlinNativeTarget> {
        binaries {
            executable {
                entryPoint = "org.basaltmc.proxy.main"
            }
        }
    }
}

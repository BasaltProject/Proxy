/**
 * The version is `<minecraft>.<build>`: what an operator needs to know is which Minecraft this
 * speaks, and the build number is how far along that line it is. The CI passes the number; anything
 * built anywhere else is build 0 and says so, because it is not a build anyone can ask for.
 */
val minecraftVersion: String = providers.gradleProperty("basalt.minecraft").get()
val buildNumber: String =
    providers.gradleProperty("basalt.build").orNull
        ?: System.getenv("BASALT_BUILD")
        ?: "0"
val basaltVersion: String = if (buildNumber == "0") "$minecraftVersion.0-dev" else "$minecraftVersion.$buildNumber"

allprojects {
    group = "org.basaltmc"
    version = basaltVersion
    extra["minecraftVersion"] = minecraftVersion
}

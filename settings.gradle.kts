rootProject.name = "proxy-runtime"

pluginManagement {
    // Wire owns every build convention, this repository included: see its build-logic for why
    // there is only one of these.
    includeBuild("wire/build-logic")
    repositories {
        gradlePluginPortal()
        mavenCentral()
    }
}

dependencyResolutionManagement {
    repositories {
        mavenCentral()
    }
    versionCatalogs {
        // One catalog for every repository that consumes Wire, so versions cannot drift apart.
        create("libs") {
            from(files("wire/gradle/libs.versions.toml"))
        }
    }
}

include(
    ":wire-common",
    ":wire-nbt",
    ":wire-text",
    ":wire-io",
    ":wire-protocol",
    ":wire-auth",
    ":wire-multiversion",
    ":wire-bedrock",
    ":proxy-runtime",
)

// The network layer is shared with the server and lives in the Wire submodule. It is composed in
// rather than depended on as a published artifact: both repositories move together for now.
for (name in listOf(
    "wire-common",
    "wire-nbt",
    "wire-text",
    "wire-io",
    "wire-protocol",
    "wire-auth",
    "wire-multiversion",
    "wire-bedrock",
)) {
    project(":$name").projectDir = file("wire/$name")
}

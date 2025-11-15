basalt_module(proxy-runtime-core
    SOURCES
        src/Cli.cpp
    DEPENDS basalt::wire-common basalt::wire-io basalt::wire-multiversion
)

target_compile_definitions(proxy-runtime-core PUBLIC BASALT_VERSION="${BASALT_VERSION}")

# The binary is named the way it is downloaded, the same as the server's.
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(_platform "macOS")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(_platform "Windows")
else()
    set(_platform "${CMAKE_SYSTEM_NAME}")
endif()
if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    set(_arch "arm64")
else()
    set(_arch "x64")
endif()

add_executable(basalt-proxy src/Main.cpp)
# wire-io comes through the core already; naming it twice makes the linker say so.
target_link_libraries(basalt-proxy PRIVATE basalt::proxy-runtime-core)
set_target_properties(basalt-proxy PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    OUTPUT_NAME "BasaltProxy-${BASALT_VERSION}-${_platform}-${_arch}"
)

basalt_test(proxy-runtime-cli-test
    SOURCES
        test/CliTest.cpp
    DEPENDS basalt::proxy-runtime-core
)

basalt_module(proxy-runtime-core
    SOURCES
        src/Cli.cpp
        src/Admission.cpp
        src/BackendRoutes.cpp
        src/BackendAttempts.cpp
        src/BackendDial.cpp
        src/BackendLogin.cpp
        src/ClientSession.cpp
        src/ClientSessionBackend.cpp
        src/PendingCookies.cpp
        src/CookieCache.cpp
        src/IdentityRegistry.cpp
        src/ProxyRuntime.cpp
    DEPENDS basalt::proxy-config basalt::wire-common basalt::wire-io basalt::wire-protocol basalt::wire-text
            basalt::wire-multiversion
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

basalt_test(proxy-runtime-network-test
    SOURCES
        test/NetworkTest.cpp
    DEPENDS basalt::proxy-runtime-core basalt::wire-protocol
)

basalt_test(proxy-runtime-admission-test
    SOURCES test/AdmissionTest.cpp
    DEPENDS basalt::proxy-runtime-core
)

basalt_test(proxy-runtime-session-test
    SOURCES test/RuntimeTest.cpp test/IdentitySessionTest.cpp
    DEPENDS basalt::proxy-runtime-core basalt::wire-auth-test-support
)
if(BASALT_BUILD_TESTS)
    set_tests_properties(proxy-runtime-session-test PROPERTIES TIMEOUT 30)
endif()

basalt_test(proxy-runtime-backend-selection-test
    SOURCES test/BackendSelectionTest.cpp
    DEPENDS basalt::proxy-runtime-core
)

basalt_test(proxy-runtime-backend-login-test
    SOURCES test/BackendLoginTest.cpp test/CookieRelayTest.cpp test/TransferRelayTest.cpp test/CookieCacheTest.cpp test/ClientSettingsTest.cpp
    DEPENDS basalt::proxy-runtime-core basalt::wire-auth-test-support
)
if(BASALT_BUILD_TESTS)
    set_tests_properties(proxy-runtime-backend-login-test PROPERTIES TIMEOUT 30)
endif()

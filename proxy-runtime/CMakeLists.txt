basalt_module(proxy-runtime-core
    SOURCES
        src/BuildInfo.cpp
        src/Cli.cpp
        src/AccessControl.cpp
        src/BuiltinAccessCommands.cpp
        src/BuiltinCommands.cpp
        src/CommandDispatcher.cpp
        src/CommandService.cpp
        src/Console.cpp
        src/Messages.cpp
        src/OnlineRoster.cpp
        src/PermissionService.cpp
        src/AccessPolicy.cpp
        src/Admission.cpp
        src/QueryServer.cpp
        src/BedrockStatus.cpp
        src/bedrock/BedrockLanguage.cpp
        src/bedrock/JavaLink.cpp
        src/bedrock/BedrockEntities.cpp
        src/bedrock/BedrockInput.cpp
        src/bedrock/BedrockMessages.cpp
        src/bedrock/BedrockPlayerState.cpp
        src/bedrock/BedrockSession.cpp
        src/bedrock/BedrockWindows.cpp
        src/bedrock/BedrockWorld.cpp
        src/BackendChangeRequest.cpp
        src/BackendRoutes.cpp
        src/BackendAttempts.cpp
        src/BackendDial.cpp
        src/BackendLogin.cpp
        src/BackendLoginQueries.cpp
        src/BackendLoginIdentity.cpp
        src/BackendLoginCookies.cpp
        src/ClientSession.cpp
        src/ClientSessionExploits.cpp
        src/ClientSessionLimbo.cpp
        src/antibot/AttackReport.cpp
        src/antibot/TcpProbe.cpp
        src/antibot/NbtShape.cpp
        src/antibot/TextScan.cpp
        src/antibot/Antibot.cpp
        src/antibot/Captcha.cpp
        src/antibot/LimboAssets.cpp
        src/antibot/LimboRegistries.cpp
        src/antibot/LimboRun.cpp
        src/ClientSessionRouting.cpp
        src/ClientSessionTranslation.cpp
        src/ClientSessionChat.cpp
        src/ClientSessionUi.cpp
        src/ClientSessionHandshake.cpp
        src/IdRenumbering.cpp
        src/ForwardingKey.cpp
        src/ClientSessionBackend.cpp
        src/ClientSessionMessages.cpp
        src/ClientSessionResourcePacks.cpp
        src/ResourcePacks.cpp
        src/ClientSessionCookies.cpp
        src/ClientSessionCandidate.cpp
        src/ClientSessionDecision.cpp
        src/ClientSessionRecovery.cpp
        src/ClientSessionKeepAlives.cpp
        src/ClientSessionPackets.cpp
        src/PendingResponses.cpp
        src/RouteVersions.cpp
        src/PendingCookies.cpp
        src/CookieCache.cpp
        src/IdentityRegistry.cpp
        src/DefaultIcon.cpp
        src/PlayerDirectory.cpp
        src/ServerListService.cpp
        src/UiService.cpp
        src/Inspection.cpp
        src/ProxyRuntime.cpp
        src/UpdateChecker.cpp
    DEPENDS basalt::proxy-config basalt::wire-common basalt::wire-io basalt::wire-protocol basalt::wire-text
            basalt::wire-multiversion basalt::wire-auth basalt::wire-bedrock basalt::wire-config basalt::wire-storage
            basalt::wire-plugins
)

# Only this file sees the version, so stamping a new one does not recompile (or invalidate the cache of) the rest.
set_source_files_properties(src/BuildInfo.cpp PROPERTIES COMPILE_DEFINITIONS BASALT_VERSION="${BASALT_VERSION}")

# The binary is named the way it is downloaded, the same as the server's.
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(_platform "macOS")
elseif(BASALT_LINUX_MUSL)
    set(_platform "Linux-musl")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(_platform "Windows")
else()
    set(_platform "${CMAKE_SYSTEM_NAME}")
endif()
set(_processor "${CMAKE_SYSTEM_PROCESSOR}")
if(APPLE AND CMAKE_OSX_ARCHITECTURES)
    set(_processor "${CMAKE_OSX_ARCHITECTURES}")
endif()
string(TOLOWER "${_processor}" _processor)
if(_processor MATCHES "^(arm64|aarch64)$")
    set(_arch "arm64")
elseif(_processor MATCHES "^(x86_64|amd64|x64)$")
    set(_arch "x64")
else()
    message(FATAL_ERROR "Unsupported release architecture: ${_processor}")
endif()

add_executable(basalt-proxy src/Main.cpp)
if(ANDROID)
    target_link_options(basalt-proxy PRIVATE $<$<CONFIG:Release>:-Wl,--strip-debug>)
endif()
# wire-io comes through the core already; naming it twice makes the linker say so.
target_link_libraries(basalt-proxy PRIVATE basalt::proxy-runtime-core basalt::proxy-plugins basalt::proxy-remote)
if(TARGET basalt::wire-shim)
    target_link_libraries(basalt-proxy PRIVATE basalt::proxy-bungee basalt::proxy-velocity)
    target_compile_definitions(basalt-proxy PRIVATE BASALT_HAS_SHIM=1)
endif()
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

basalt_test(proxy-runtime-access-test
    SOURCES test/AccessTest.cpp
    DEPENDS basalt::proxy-runtime-core basalt::wire-auth-test-support
)


basalt_test(proxy-runtime-admission-test
    SOURCES test/AdmissionTest.cpp
    DEPENDS basalt::proxy-runtime-core
)

basalt_test(proxy-runtime-session-test
    SOURCES test/RuntimeTest.cpp
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
    SOURCES
        test/BackendLoginTest.cpp
        test/CookieCacheTest.cpp
        test/CandidateSessionTest.cpp
    DEPENDS basalt::proxy-runtime-core basalt::wire-auth-test-support
)
if(BASALT_BUILD_TESTS)
    set_tests_properties(proxy-runtime-backend-login-test PROPERTIES TIMEOUT 30)
endif()

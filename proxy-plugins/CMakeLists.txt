basalt_module(proxy-plugins
    SOURCES
        src/BungeeMessaging.cpp
        src/JarDetector.cpp
        src/PluginApi.cpp
        src/PluginDecisions.cpp
        src/PluginMessagingApi.cpp
        src/PluginHudApi.cpp
        src/PluginHost.cpp
    DEPENDS basalt::proxy-runtime-core basalt::wire-lua basalt::wire-plugins basalt::wire-sqlite basalt::wire-storage
    PRIVATE_DEPENDS basalt::lua
)

if(TARGET basalt::wire-shim)
    target_sources(proxy-plugins PRIVATE src/JavaAdapter.cpp)
    target_link_libraries(proxy-plugins PUBLIC basalt::wire-shim)
    target_compile_definitions(proxy-plugins PUBLIC BASALT_HAS_SHIM=1)
endif()

basalt_test(proxy-plugins-test
    SOURCES
        test/PluginHostTest.cpp
    DEPENDS basalt::proxy-plugins
)
if(TARGET proxy-plugins-test)
    target_compile_definitions(proxy-plugins-test PRIVATE BASALT_TEST_SCRATCH_DIR="test-data")
endif()

basalt_module(proxy-plugins
    SOURCES
        src/PluginApi.cpp
        src/PluginHost.cpp
    DEPENDS basalt::proxy-runtime-core basalt::wire-lua basalt::wire-plugins basalt::wire-sqlite
    PRIVATE_DEPENDS basalt::lua
)

basalt_test(proxy-plugins-test
    SOURCES
        test/PluginHostTest.cpp
    DEPENDS basalt::proxy-plugins
)
target_compile_definitions(proxy-plugins-test PRIVATE BASALT_TEST_SCRATCH_DIR="${CMAKE_CURRENT_BINARY_DIR}/test-data")

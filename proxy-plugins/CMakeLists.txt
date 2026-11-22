basalt_wasm_host_registry(_wasm_imports
    DIR ${CMAKE_CURRENT_SOURCE_DIR}/src
    PREFIX WasmImports
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/WasmImportsRegistry.cpp
    INCLUDES WasmImports.hpp
    NAMESPACE basalt::proxy::plugins::wasmimport
    ENTRY "void addAllImports(std::vector<Entry>& out)"
    DECLARE "void add@Area@Imports(std::vector<Entry>& out)"
    CALL "add@Area@Imports(out)")

basalt_module(proxy-plugins
    SOURCES
        src/BungeeMessaging.cpp
        src/JarDetector.cpp
        src/PluginDecisions.cpp
        src/PluginSupport.cpp
        src/PluginHost.cpp
        src/PluginHostEvents.cpp
        src/PluginHostMessaging.cpp
        src/WasmEventFields.cpp
        src/WasmHost.cpp
        src/WasmImports.cpp
        ${_wasm_imports}
        src/WasmLoad.cpp
        src/WasmNetwork.cpp
    DEPENDS basalt::proxy-runtime-core basalt::wire-wasm basalt::wire-permissions basalt::wire-plugins basalt::wire-sqlite basalt::wire-storage
)

if(TARGET basalt::wire-shim)
    target_sources(proxy-plugins PRIVATE src/JavaAdapter.cpp src/JavaHud.cpp src/JavaPermissions.cpp)
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

# The commented defaults an operator gets on first run, compiled in rather than read from a folder
# beside the binary: a release is one self-contained executable.
file(GLOB BASALT_CONFIG_DEFAULTS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/resources/config-defaults/*.toml)
basalt_embed_text(proxy-config-defaults
    NAMESPACE basalt::proxy::config
    HEADER basalt/proxy/config/ConfigDefaults.hpp
    SUFFIX DefaultToml
    FILES ${BASALT_CONFIG_DEFAULTS}
)

basalt_module(proxy-config
    SOURCES src/ProxyConfig.cpp src/ServersConfig.cpp src/ConfigFiles.cpp src/AddressRange.cpp src/RemoteConfig.cpp src/AntibotConfig.cpp
    DEPENDS basalt::wire-common basalt::wire-config basalt::wire-io basalt::wire-auth basalt::wire-protocol
            basalt::proxy-config-defaults
)

basalt_test(proxy-config-test
    SOURCES test/ConfigTest.cpp
    DEPENDS basalt::proxy-config
)

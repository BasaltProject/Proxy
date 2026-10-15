basalt_module(proxy-config
    SOURCES src/ProxyConfig.cpp src/BackendConfig.cpp src/ConfigDefaults.cpp
    DEPENDS basalt::wire-common basalt::wire-io basalt::wire-auth
)

basalt_test(proxy-config-test
    SOURCES test/ConfigTest.cpp
    DEPENDS basalt::proxy-config
)

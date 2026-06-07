basalt_module(proxy-remote
    SOURCES
        src/RemoteApi.cpp
        src/RemoteJson.cpp
    DEPENDS basalt::proxy-config basalt::proxy-runtime-core basalt::wire-common basalt::wire-remote
            basalt::wire-multiversion
)


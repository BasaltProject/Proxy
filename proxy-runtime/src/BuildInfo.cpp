#include <basalt/proxy/runtime/BuildInfo.hpp>

#ifndef BASALT_VERSION
#define BASALT_VERSION "0.0.0-dev"
#endif

namespace basalt::proxy::runtime::buildInfo {

const std::string_view kVersion = BASALT_VERSION;

} // namespace basalt::proxy::runtime::buildInfo

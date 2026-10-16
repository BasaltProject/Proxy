#include "RouteVersions.hpp"

#include <basalt/proxy/runtime/BuildInfo.hpp>
#include <basalt/wire/multiversion/ProtocolVersions.hpp>

#include <algorithm>
#include <array>

namespace basalt::proxy::runtime::detail {

bool translatedClientVerified(std::int32_t protocol) noexcept {
    namespace versions = wire::multiversion::protocolVersions;
    constexpr std::array<std::int32_t, 34> kVerified = {
        versions::kV1_9,    versions::kV1_9_2,   versions::kV1_11,   versions::kV1_12,   versions::kV1_12_1,
        versions::kV1_12_2, versions::kV1_13,    versions::kV1_13_1, versions::kV1_13_2, versions::kV1_14,
        versions::kV1_14_1, versions::kV1_14_3,  versions::kV1_14_4, versions::kV1_15,   versions::kV1_15_1,
        versions::kV1_15_2, versions::kV1_16,    versions::kV1_16_1, versions::kV1_16_2, versions::kV1_16_3,
        versions::kV1_17,   versions::kV1_17_1,  versions::kV1_18_2, versions::kV1_19,   versions::kV1_19_3,
        versions::kV1_19_4, versions::kV1_20_2,  versions::kV1_21,   versions::kV1_21_4, versions::kV1_21_5,
        versions::kV1_21_6, versions::kV1_21_11, versions::kV26_1,   versions::kV26_2};
    return std::ranges::find(kVerified, protocol) != kVerified.end();
}

std::expected<void, RouteRefusal> resolveRoute(std::int32_t clientProtocol, std::int32_t backendProtocol) noexcept {
    constexpr std::int32_t kNative = buildInfo::kProtocolVersion;
    if (backendProtocol != kNative)
        return std::unexpected(clientProtocol == kNative ? RouteRefusal::kBackendUnsupported
                                                         : RouteRefusal::kNoTranslation);
    if (clientProtocol == kNative || translatedClientVerified(clientProtocol))
        return {};
    return std::unexpected(RouteRefusal::kClientUnsupported);
}

} // namespace basalt::proxy::runtime::detail

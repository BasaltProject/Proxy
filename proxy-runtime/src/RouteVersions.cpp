#include "RouteVersions.hpp"

#include <basalt/proxy/runtime/BuildInfo.hpp>
#include <basalt/wire/multiversion/ProtocolVersions.hpp>

#include <algorithm>
#include <array>

namespace basalt::proxy::runtime::detail {

bool translatedClientVerified(std::int32_t protocol) noexcept {
    namespace versions = wire::multiversion::protocolVersions;
    constexpr std::array<std::int32_t, 35> kVerified = {
        versions::kV1_8,    versions::kV1_9,    versions::kV1_9_2,   versions::kV1_11,   versions::kV1_12,
        versions::kV1_12_1, versions::kV1_12_2, versions::kV1_13,    versions::kV1_13_1, versions::kV1_13_2,
        versions::kV1_14,   versions::kV1_14_1, versions::kV1_14_3,  versions::kV1_14_4, versions::kV1_15,
        versions::kV1_15_1, versions::kV1_15_2, versions::kV1_16,    versions::kV1_16_1, versions::kV1_16_2,
        versions::kV1_16_3, versions::kV1_17,   versions::kV1_17_1,  versions::kV1_18_2, versions::kV1_19,
        versions::kV1_19_3, versions::kV1_19_4, versions::kV1_20_2,  versions::kV1_21,   versions::kV1_21_4,
        versions::kV1_21_5, versions::kV1_21_6, versions::kV1_21_11, versions::kV26_1,   versions::kV26_2};
    return std::ranges::find(kVerified, protocol) != kVerified.end();
}

bool backendVerified(std::int32_t protocol) noexcept {
    constexpr std::array<std::int32_t, 38> kVerified = {47,  110, 210, 316, 340, 393, 401, 404, 480, 490, 498, 573, 575,
                                                        578, 751, 753, 754, 755, 756, 757, 758, 759, 760, 761, 762, 763,
                                                        765, 766, 767, 768, 769, 770, 771, 772, 773, 774, 775, 776};
    return std::ranges::find(kVerified, protocol) != kVerified.end();
}

LoginFinishedTail loginFinishedTail(std::int32_t protocol) noexcept {
    namespace versions = wire::multiversion::protocolVersions;
    if (protocol >= versions::kV26_2)
        return LoginFinishedTail::kSessionId;
    return protocol >= versions::kV1_21_2 || protocol < versions::kV1_20_5 ? LoginFinishedTail::kNone
                                                                           : LoginFinishedTail::kStrictErrorsFlag;
}

std::expected<void, RouteRefusal> resolveRoute(std::int32_t clientProtocol, std::int32_t backendProtocol) noexcept {
    constexpr std::int32_t kNative = buildInfo::kProtocolVersion;
    if (backendProtocol != kNative) {
        if (backendProtocol == clientProtocol && backendVerified(backendProtocol))
            return {};
        return std::unexpected(clientProtocol == kNative ? RouteRefusal::kBackendUnsupported
                                                         : RouteRefusal::kNoTranslation);
    }
    if (clientProtocol == kNative || translatedClientVerified(clientProtocol))
        return {};
    return std::unexpected(RouteRefusal::kClientUnsupported);
}

} // namespace basalt::proxy::runtime::detail

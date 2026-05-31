#pragma once

#include <basalt/proxy/runtime/ProxyRuntime.hpp>

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace basalt::proxy::runtime::detail {

/// Who is in play and on which backend, kept from the player events of both the Java and the Bedrock side.
class OnlineRoster {
public:
    struct Entry {
        std::string name;
        std::string uuid;
        std::string backend;
    };

    void apply(const PlayerEvent& event);

    [[nodiscard]] std::vector<Entry> all() const;

    [[nodiscard]] std::optional<Entry> find(std::string_view name) const;

    [[nodiscard]] std::size_t count() const;

private:
    mutable std::mutex mutex_;
    std::map<std::string, Entry, std::less<>> byName_;
};

} // namespace basalt::proxy::runtime::detail

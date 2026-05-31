#include "OnlineRoster.hpp"

#include <basalt/wire/common/placeholder/Placeholders.hpp>

namespace basalt::proxy::runtime::detail {
namespace {
using wire::common::placeholder::lowercaseAscii;
}

void OnlineRoster::apply(const PlayerEvent& event) {
    const std::lock_guard lock{mutex_};
    const std::string key = lowercaseAscii(event.player);
    switch (event.kind) {
    case PlayerEventKind::kJoin:
    case PlayerEventKind::kSwitch:
        byName_[key] = Entry{event.player, event.uuid, event.backend};
        break;
    case PlayerEventKind::kLeave:
        byName_.erase(key);
        break;
    default:
        break;
    }
}

std::vector<OnlineRoster::Entry> OnlineRoster::all() const {
    const std::lock_guard lock{mutex_};
    std::vector<Entry> entries;
    for (const auto& [key, entry] : byName_)
        entries.push_back(entry);
    return entries;
}

std::optional<OnlineRoster::Entry> OnlineRoster::find(std::string_view name) const {
    const std::lock_guard lock{mutex_};
    const auto found = byName_.find(lowercaseAscii(name));
    return found == byName_.end() ? std::nullopt : std::optional{found->second};
}

std::size_t OnlineRoster::count() const {
    const std::lock_guard lock{mutex_};
    return byName_.size();
}

} // namespace basalt::proxy::runtime::detail

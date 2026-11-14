#include "JavaPermissions.hpp"

#include <shared_mutex>

namespace basalt::proxy::plugins {
namespace permissions = wire::permissions;

struct JavaPermissions::Seat {
    std::shared_mutex gate;
    bool alive = true;
    Ask ask;
    AskMeta askMeta;
    permissions::ProviderHandle handle;
    permissions::ProviderHandle metaHandle;
};

JavaPermissions::JavaPermissions(const PluginHooks& hooks, std::string id, std::int32_t priority, Ask ask,
                                 AskMeta askMeta, Locate locate)
    : hooks_(&hooks), id_(std::move(id)), priority_(priority), locate_(std::move(locate)),
      seat_(std::make_shared<Seat>()) {
    seat_->ask = std::move(ask);
    seat_->askMeta = std::move(askMeta);
}

JavaPermissions::~JavaPermissions() { retire(); }

void JavaPermissions::retire() {
    permissions::ProviderHandle handle;
    permissions::ProviderHandle metaHandle;
    {
        const std::unique_lock lock{seat_->gate};
        seat_->alive = false;
        handle = std::move(seat_->handle);
        metaHandle = std::move(seat_->metaHandle);
    }
    handle.remove();
    metaHandle.remove();
}

void JavaPermissions::provideMeta(bool active) {
    permissions::ProviderHandle previous;
    {
        const std::unique_lock lock{seat_->gate};
        previous = std::move(seat_->metaHandle);
    }
    previous.remove();
    if (!active || hooks_->permissions == nullptr)
        return;
    const std::weak_ptr<Seat> weak = seat_;
    auto handle = hooks_->permissions->provide(
        {.id = id_ + ":meta",
         .priority = priority_,
         .check = {},
         .meta = [weak](const permissions::Subject& subject, std::string_view key,
                        const permissions::Context&) -> std::optional<std::string> {
             const auto seat = weak.lock();
             if (!seat)
                 return std::nullopt;
             const std::shared_lock lock{seat->gate};
             if (!seat->alive)
                 return std::nullopt;
             return seat->askMeta(subject.isConsole() ? std::string{} : std::string{subject.name}, std::string{key});
         }});
    const std::unique_lock lock{seat_->gate};
    if (seat_->alive)
        seat_->metaHandle = std::move(handle);
}

int JavaPermissions::value(const std::string& player, const std::string& node) const {
    if (hooks_->permissions == nullptr)
        return player.empty() || (hooks_->hasPermission && hooks_->hasPermission(player, node)) ? 1 : 0;
    auto& chain = *hooks_->permissions;
    std::optional<std::pair<std::string, std::string>> online;
    if (!player.empty() && !chain.onlyDefault() && locate_)
        online = locate_(player);
    const auto subject = player.empty()
                             ? permissions::Subject::console()
                             : permissions::Subject::player(online ? std::string_view{online->first} : "", player);
    const permissions::Context context{online ? std::string_view{online->second} : "", {}, {}};
    switch (chain.check(subject, node, context)) {
    case permissions::Tristate::kTrue:
        return 1;
    case permissions::Tristate::kFalse:
        return 0;
    case permissions::Tristate::kUndefined:
        break;
    }
    return -1;
}

void JavaPermissions::provide(bool active, bool withRest) {
    permissions::ProviderHandle previous;
    {
        const std::unique_lock lock{seat_->gate};
        previous = std::move(seat_->handle);
    }
    previous.remove();
    if (!active || hooks_->permissions == nullptr)
        return;
    const std::weak_ptr<Seat> weak = seat_;
    auto handle = hooks_->permissions->provide(
        {.id = id_,
         .priority = priority_,
         .check =
             [weak, withRest](const permissions::Query& query) {
                 const auto seat = weak.lock();
                 if (!seat)
                     return permissions::Tristate::kUndefined;
                 int rest = -1;
                 if (withRest) {
                     const auto below = query.rest();
                     rest = below == permissions::Tristate::kUndefined
                                ? -1
                                : (below == permissions::Tristate::kTrue ? 1 : 0);
                 }
                 const std::shared_lock lock{seat->gate};
                 if (!seat->alive)
                     return permissions::Tristate::kUndefined;
                 const std::string subject = query.subject().isConsole() ? "" : std::string{query.subject().name};
                 const int answer = seat->ask(subject, std::string{query.node()}, rest);
                 return answer < 0 ? permissions::Tristate::kUndefined : permissions::tristateOf(answer == 1);
             },
         .meta = {}});
    const std::unique_lock lock{seat_->gate};
    if (seat_->alive)
        seat_->handle = std::move(handle);
}

} // namespace basalt::proxy::plugins

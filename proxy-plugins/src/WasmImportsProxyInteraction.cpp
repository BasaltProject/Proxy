#include "WasmImports.hpp"
#include "WasmInteraction.hpp"

#include <basalt/wire/common/Uuid.hpp>
#include <basalt/wire/text/MiniMessage.hpp>

#include <algorithm>
#include <array>
#include <atomic>

namespace basalt::proxy::plugins {

namespace {
constexpr std::size_t kMaxDialogBody = 32;
constexpr std::size_t kMaxDialogButtons = 16;
constexpr std::size_t kMaxButtonCommand = 256;
constexpr std::size_t kMaxCookieKey = 256;
constexpr std::size_t kMaxCookieBytes = 5120;
constexpr std::size_t kMaxPendingCookies = 256;
constexpr std::array<std::string_view, 8> kPackStatuses{
    "successfully_loaded", "declined",    "failed_download", "accepted",
    "downloaded",          "invalid_url", "failed_reload",   "discarded"};

std::atomic<std::uint64_t> gNextSerial{1};

void notifyWatchers(WasmHost& host, std::vector<std::int32_t> InteractionState::* list,
                    const wire::wasm::Value& value) {
    for (auto& plugin : host.plugins_) {
        if (plugin->broken)
            continue;
        const std::vector<std::int32_t> tokens = (interactionState(*plugin).*list);
        for (const std::int32_t token : tokens)
            if (!deliverValue(*plugin, token, value))
                break;
    }
}

[[nodiscard]] wire::wasm::Value playerMap(WasmHost& host, std::string_view name) {
    wire::wasm::Value::Map map;
    map.push_back({"player", std::int64_t{host.handleOf(name)}});
    map.push_back({"player_name", std::string{name}});
    return wire::wasm::Value{std::move(map)};
}
} // namespace

InteractionState& interactionState(WasmPlugin& plugin) {
    auto& state = areaState<InteractionState>(plugin, kInteractionArea);
    if (state.serial == 0)
        state.serial = gNextSerial.fetch_add(1);
    return state;
}

wire::wasm::Value settingsValue(const runtime::ClientSettings& settings) {
    constexpr std::array<std::string_view, 3> kChatModes{"full", "system", "hidden"};
    constexpr std::array<std::string_view, 3> kParticles{"all", "decreased", "minimal"};
    const auto pick = [](const auto& names, std::int32_t value) {
        return std::string{names[static_cast<std::size_t>(std::clamp<std::int32_t>(value, 0, 2))]};
    };
    wire::wasm::Value::Map map;
    map.push_back({"locale", settings.locale});
    map.push_back({"view_distance", std::int64_t{settings.viewDistance}});
    map.push_back({"chat_mode", pick(kChatModes, settings.chatMode)});
    map.push_back({"chat_colors", settings.chatColors});
    map.push_back({"skin_parts", std::int64_t{settings.skinParts}});
    map.push_back({"main_hand", std::string{settings.mainHand == 0 ? "left" : "right"}});
    map.push_back({"text_filtering", settings.textFiltering});
    map.push_back({"server_listing", settings.serverListing});
    map.push_back({"particles", pick(kParticles, settings.particles)});
    return wire::wasm::Value{std::move(map)};
}

void notifySettings(WasmHost& host, const WasmHost::Player& player) {
    if (!player.settings)
        return;
    wire::wasm::Value event = playerMap(host, player.name);
    std::get<wire::wasm::Value::Map>(event.data).push_back({"settings", settingsValue(*player.settings)});
    notifyWatchers(host, &InteractionState::settingsWatchers, event);
}

void notifyResourcePack(WasmHost& host, std::string_view player, std::string_view id, std::int64_t status) {
    if (status < 0 || status >= std::ssize(kPackStatuses) || host.handleOf(player) == 0)
        return;
    wire::wasm::Value event = playerMap(host, player);
    auto& map = std::get<wire::wasm::Value::Map>(event.data);
    map.push_back({"id", std::string{id}});
    map.push_back({"status", std::string{kPackStatuses[static_cast<std::size_t>(status)]}});
    notifyWatchers(host, &InteractionState::packWatchers, event);
}

namespace wasmimport {
namespace {

using namespace wire::wasm;
using wire::wasm::Value;

[[nodiscard]] const std::string* stringOf(const Value& map, std::string_view key) {
    const Value* found = map.find(key);
    return found != nullptr ? found->asString() : nullptr;
}

[[nodiscard]] std::int32_t handed(WasmHost& host, std::int32_t handle, const WasmHost::Player*& player) {
    player = host.playerOf(handle);
    return player == nullptr ? kErrStale : kOk;
}

[[nodiscard]] std::int32_t sendHud(WasmHost& host, const WasmHost::Player& player, runtime::PlayerHud action) {
    return host.hooks().hud && host.hooks().hud(player.name, std::move(action)) ? kOk : kErrInvalid;
}

void interactionWatch(void* user, Call& call) {
    WasmPlugin& plugin = self(user);
    const auto name = text(call, 0);
    if (!name || (*name != "player.resource_pack" && *name != "player.settings"))
        return call.ret(kErrInvalid);
    InteractionState& state = interactionState(plugin);
    (*name == "player.settings" ? state.settingsWatchers : state.packWatchers).push_back(call.i32(2));
    call.ret(kOk);
}

[[nodiscard]] bool dialogOf(const WasmPlugin& plugin, const Value& value, runtime::HudDialog& dialog) {
    if (value.asMap() == nullptr)
        return false;
    const Value* kind = value.find("kind");
    if (kind != nullptr) {
        const std::string* word = kind->asString();
        if (word == nullptr)
            return false;
        if (*word == "notice")
            dialog.kind = runtime::HudDialog::Kind::kNotice;
        else if (*word == "confirmation")
            dialog.kind = runtime::HudDialog::Kind::kConfirmation;
        else if (*word == "multi_action")
            dialog.kind = runtime::HudDialog::Kind::kMultiAction;
        else
            return false;
    }
    if (const Value* title = value.find("title")) {
        if (title->asString() == nullptr)
            return false;
        dialog.title = componentOf(plugin, *title->asString());
    }
    if (const Value* body = value.find("body"); body != nullptr && !body->isNull()) {
        if (const std::string* line = body->asString()) {
            dialog.body.push_back(componentOf(plugin, *line));
        } else if (const auto* lines = body->asList(); lines != nullptr && lines->size() <= kMaxDialogBody) {
            for (const Value& item : *lines) {
                if (item.asString() == nullptr)
                    return false;
                dialog.body.push_back(componentOf(plugin, *item.asString()));
            }
        } else {
            return false;
        }
    }
    if (const Value* buttons = value.find("buttons"); buttons != nullptr && !buttons->isNull()) {
        const auto* list = buttons->asList();
        if (list == nullptr || list->size() > kMaxDialogButtons)
            return false;
        for (const Value& item : *list) {
            const std::string* label = stringOf(item, "label");
            const std::string* command = stringOf(item, "command");
            if (label == nullptr || command == nullptr || command->size() > kMaxButtonCommand)
                return false;
            dialog.buttons.push_back({componentOf(plugin, *label), *command});
        }
    }
    if (const Value* escape = value.find("can_close_with_escape")) {
        if (escape->asBool() == nullptr)
            return false;
        dialog.canCloseWithEscape = *escape->asBool();
    }
    return true;
}

void dialogShow(void* user, Call& call) {
    WasmPlugin& plugin = self(user);
    const WasmHost::Player* player = nullptr;
    if (const auto status = handed(*plugin.host, call.i32(0), player); status != kOk)
        return call.ret(status);
    const auto value = readValue(call.memory(), call.i32(1), call.i32(2));
    runtime::HudDialog dialog;
    if (!value || !dialogOf(plugin, *value, dialog))
        return call.ret(kErrInvalid);
    call.ret(sendHud(*plugin.host, *player, std::move(dialog)));
}

void dialogClear(void* user, Call& call) {
    WasmHost& host = *self(user).host;
    const WasmHost::Player* player = nullptr;
    if (const auto status = handed(host, call.i32(0), player); status != kOk)
        return call.ret(status);
    runtime::HudDialog dialog;
    dialog.kind = runtime::HudDialog::Kind::kClear;
    call.ret(sendHud(host, *player, std::move(dialog)));
}

[[nodiscard]] wire::common::Uuid packIdOf(std::string_view given, std::string_view fallback) {
    if (const auto parsed = wire::common::Uuid::parse(given))
        return *parsed;
    return wire::common::Uuid::nameBased("basalt:resource_pack:" + std::string{given.empty() ? fallback : given});
}

[[nodiscard]] bool hexHash(std::string_view hash) {
    return hash.empty() || (hash.size() == 40 && std::ranges::all_of(hash, [](char c) {
                                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
                            }));
}

void resourcePackPush(void* user, Call& call) {
    WasmPlugin& plugin = self(user);
    const WasmHost::Player* player = nullptr;
    if (const auto status = handed(*plugin.host, call.i32(0), player); status != kOk)
        return call.ret(status);
    const auto value = readValue(call.memory(), call.i32(1), call.i32(2));
    if (!value || value->asMap() == nullptr)
        return call.ret(kErrInvalid);
    const std::string* url = stringOf(*value, "url");
    const std::string* hash = stringOf(*value, "hash");
    const std::string* given = stringOf(*value, "id");
    const std::string* prompt = stringOf(*value, "prompt");
    const Value* required = value->find("required");
    if (url == nullptr || url->empty() || (hash != nullptr && !hexHash(*hash)) ||
        (required != nullptr && required->asBool() == nullptr))
        return call.ret(kErrInvalid);
    runtime::HudResourcePack pack;
    pack.url = *url;
    pack.hash = hash != nullptr ? *hash : std::string{};
    pack.required = required != nullptr && *required->asBool();
    if (prompt != nullptr && !prompt->empty())
        pack.prompt = componentOf(plugin, *prompt);
    pack.id = packIdOf(given != nullptr ? std::string_view{*given} : std::string_view{}, *url);
    const std::string id = pack.id.toString();
    if (const auto status = sendHud(*plugin.host, *player, std::move(pack)); status != kOk)
        return call.ret(status);
    call.ret(call.memory().put(call.i32(3), call.i32(4), id));
}

void resourcePackRemove(void* user, Call& call) {
    WasmHost& host = *self(user).host;
    const WasmHost::Player* player = nullptr;
    const auto id = text(call, 1);
    if (const auto status = handed(host, call.i32(0), player); status != kOk)
        return call.ret(status);
    if (!id)
        return call.ret(kErrInvalid);
    runtime::HudResourcePack pack;
    pack.op = runtime::HudResourcePack::Op::kPop;
    pack.id = packIdOf(*id, *id);
    call.ret(sendHud(host, *player, std::move(pack)));
}

void resourcePackClear(void* user, Call& call) {
    WasmHost& host = *self(user).host;
    const WasmHost::Player* player = nullptr;
    if (const auto status = handed(host, call.i32(0), player); status != kOk)
        return call.ret(status);
    runtime::HudResourcePack pack;
    pack.op = runtime::HudResourcePack::Op::kClear;
    call.ret(sendHud(host, *player, std::move(pack)));
}

void cookieSet(void* user, Call& call) {
    WasmHost& host = *self(user).host;
    const WasmHost::Player* player = nullptr;
    const auto key = text(call, 1);
    const auto data =
        call.memory().range(static_cast<std::uint32_t>(call.i32(3)), static_cast<std::uint32_t>(call.i32(4)));
    if (const auto status = handed(host, call.i32(0), player); status != kOk)
        return call.ret(status);
    if (!key || key->empty() || key->size() > kMaxCookieKey || !data || data->size() > kMaxCookieBytes)
        return call.ret(kErrInvalid);
    runtime::HudStoreCookie cookie;
    cookie.key = std::string{*key};
    cookie.data.assign(data->begin(), data->end());
    call.ret(sendHud(host, *player, std::move(cookie)));
}

void cookieRequest(void* user, Call& call) {
    WasmPlugin& plugin = self(user);
    WasmHost& host = *plugin.host;
    const WasmHost::Player* player = nullptr;
    const auto key = text(call, 1);
    if (const auto status = handed(host, call.i32(0), player); status != kOk)
        return call.ret(status);
    if (!key || key->empty() || key->size() > kMaxCookieKey || !host.hooks().requestCookie)
        return call.ret(kErrInvalid);
    if (requests(plugin).size() >= kMaxPendingCookies)
        return call.ret(kErrAbsent);
    const std::int32_t request = beginRequest(plugin, call.i32(3));
    if (request == 0)
        return call.ret(kErrAbsent);
    const std::uint64_t serial = interactionState(plugin).serial;
    WasmPlugin* const owner = &plugin;
    PluginHost* const server = &host.host();
    const bool asked = host.hooks().requestCookie(
        player->name, *key, [server, owner, serial, request](std::optional<std::vector<std::uint8_t>> data) {
            static_cast<void>(server->postCompletion([owner, serial, request, data = std::move(data)](WasmHost& wasm) {
                const bool alive =
                    std::ranges::any_of(wasm.plugins_, [&](const auto& item) { return item.get() == owner; });
                if (!alive || interactionState(*owner).serial != serial)
                    return;
                Value value;
                if (data)
                    value = Value{Blob{*data}};
                static_cast<void>(completeRequest(*owner, request, value));
            }));
        });
    if (!asked) {
        static_cast<void>(requests(plugin).remove(request));
        return call.ret(kErrInvalid);
    }
    call.ret(request);
}

void playerTransfer(void* user, Call& call) {
    WasmHost& host = *self(user).host;
    const WasmHost::Player* player = nullptr;
    const auto target = text(call, 1);
    if (const auto status = handed(host, call.i32(0), player); status != kOk)
        return call.ret(status);
    if (!target || target->empty() || call.i32(3) < 1 || call.i32(3) > 65535)
        return call.ret(kErrInvalid);
    call.ret(sendHud(host, *player, runtime::HudTransfer{std::string{*target}, call.i32(3)}));
}

constexpr std::array kInteraction = std::to_array<Entry>({
    {kProxyModule, "interaction_watch", interactionWatch},
    {kProxyModule, "dialog_show", dialogShow},
    {kProxyModule, "dialog_clear", dialogClear},
    {kProxyModule, "resource_pack_push", resourcePackPush},
    {kProxyModule, "resource_pack_remove", resourcePackRemove},
    {kProxyModule, "resource_pack_clear", resourcePackClear},
    {kProxyModule, "cookie_set", cookieSet},
    {kProxyModule, "cookie_request", cookieRequest},
    {kProxyModule, "player_transfer", playerTransfer},
});

} // namespace

wire::text::Component componentOf(const WasmPlugin& plugin, std::string_view line) {
    const std::string resolved = line.starts_with('@')
                                     ? plugin.text->translation(line.substr(1)).value_or(std::string{line})
                                     : std::string{line};
    return wire::text::miniMessage::parse(resolved.substr(0, kWasmMaxText));
}

void addProxyInteractionImports(std::vector<Entry>& out) {
    out.insert(out.end(), kInteraction.begin(), kInteraction.end());
}

} // namespace wasmimport
} // namespace basalt::proxy::plugins

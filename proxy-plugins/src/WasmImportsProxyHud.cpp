#include "WasmImports.hpp"
#include "WasmValues.hpp"

#include <basalt/wire/text/MiniMessage.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace basalt::proxy::plugins::wasmimport {
namespace {

using namespace wire::wasm;

constexpr std::size_t kMaxIdBytes = 64;
constexpr std::size_t kMaxSidebarLines = 15;
constexpr std::size_t kMaxArgs = 64;
constexpr std::array<std::string_view, 7> kColors{"pink", "blue", "red", "green", "yellow", "purple", "white"};
constexpr std::array<std::string_view, 5> kOverlays{"progress", "notched_6", "notched_10", "notched_12", "notched_20"};
// parity: ServerLinks.KnownLinkType, by id.
constexpr std::array<std::string_view, 10> kLinkTypes{
    "report_bug", "community_guidelines", "support", "status", "feedback", "community", "website", "forums",
    "news",       "announcements"};
constexpr std::array<std::string_view, 3> kCompletionActions{"add", "remove", "set"};

[[nodiscard]] std::int32_t indexOf(const auto& names, std::string_view name, std::int32_t fallback) {
    const auto found = std::ranges::find(names, name);
    return found == names.end() ? fallback : static_cast<std::int32_t>(found - names.begin());
}

void putCount(std::string& out, std::uint32_t count) {
    std::array<char, 4> bytes{};
    std::memcpy(bytes.data(), &count, 4);
    out.append(bytes.data(), 4);
}

void putWord(std::string& out, std::string_view word) {
    putCount(out, static_cast<std::uint32_t>(word.size()));
    out.append(word);
}

/// The texts of one call: lang keys, the {name} arguments and MiniMessage in one place.
struct Texts {
    const WasmPlugin& plugin;
    std::string packed;

    [[nodiscard]] wire::text::Component parse(std::string_view raw) const {
        std::string line = raw.starts_with('@') ? plugin.text->translation(raw.substr(1)).value_or(std::string{raw})
                                                : std::string{raw};
        line.resize(std::min(line.size(), kWasmMaxText));
        return wire::text::miniMessage::parse(applyArguments(std::move(line), packed));
    }
};

/// The string under `key`: nothing when missing or null, false when it has another type.
[[nodiscard]] bool stringOf(const Value& spec, std::string_view key, std::optional<std::string_view>& out) {
    const Value* found = spec.find(key);
    if (found == nullptr || found->isNull())
        return true;
    if (found->asString() == nullptr)
        return false;
    out = *found->asString();
    return true;
}

/// The packed argument list of the optional `args` map; false when it is not a map of strings.
[[nodiscard]] bool argsOf(const Value& spec, std::string& packed) {
    const Value* found = spec.find("args");
    if (found == nullptr || found->isNull())
        return true;
    const Value::Map* map = found->asMap();
    if (map == nullptr)
        return false;
    const std::size_t count = std::min(map->size(), kMaxArgs);
    putCount(packed, static_cast<std::uint32_t>(count));
    for (std::size_t at = 0; at < count; ++at) {
        const std::string* value = (*map)[at].value.asString();
        if (value == nullptr)
            return false;
        putWord(packed, (*map)[at].key);
        putWord(packed, *value);
    }
    return true;
}

[[nodiscard]] std::expected<Value, std::int32_t> spec(Call& call, std::size_t index) {
    auto value = readValue(call.memory(), call.i32(index), call.i32(index + 1));
    if (value && value->asMap() == nullptr)
        return std::unexpected(kErrInvalid);
    return value;
}

[[nodiscard]] std::optional<std::string> idOf(Call& call, std::size_t index) {
    const auto id = text(call, index);
    if (!id || id->empty() || id->size() > kMaxIdBytes)
        return std::nullopt;
    return std::string{*id};
}

[[nodiscard]] std::int32_t deliver(WasmPlugin& plugin, const WasmHost::Player& player, runtime::PlayerHud action) {
    const auto& hud = plugin.host->hooks().hud;
    return hud && hud(player.name, std::move(action)) ? kOk : kErrAbsent;
}

/// Runs `body(plugin, player)` for a live player handle in argument 0, which returns the status.
template <typename Body>
void withPlayer(void* user, Call& call, Body body) {
    WasmPlugin& plugin = self(user);
    const WasmHost::Player* player = plugin.host->playerOf(call.i32(0));
    if (player == nullptr)
        return call.ret(kErrStale);
    call.ret(body(plugin, *player));
}

void clearTitle(void* user, Call& call) {
    withPlayer(user, call, [&](WasmPlugin& plugin, const WasmHost::Player& player) {
        runtime::HudTitle title;
        title.clear = true;
        title.reset = call.i32(1) != 0;
        return deliver(plugin, player, std::move(title));
    });
}

void tabList(void* user, Call& call) {
    withPlayer(user, call, [&](WasmPlugin& plugin, const WasmHost::Player& player) -> std::int32_t {
        const auto value = spec(call, 1);
        Texts texts{plugin, {}};
        std::optional<std::string_view> header;
        std::optional<std::string_view> footer;
        if (!value || !stringOf(*value, "header", header) || !stringOf(*value, "footer", footer) ||
            !argsOf(*value, texts.packed))
            return kErrInvalid;
        runtime::HudTabList list;
        if (header)
            list.header = texts.parse(*header);
        if (footer)
            list.footer = texts.parse(*footer);
        return deliver(plugin, player, std::move(list));
    });
}

void bossbarShow(void* user, Call& call) {
    withPlayer(user, call, [&](WasmPlugin& plugin, const WasmHost::Player& player) -> std::int32_t {
        const auto id = idOf(call, 1);
        const auto value = spec(call, 3);
        Texts texts{plugin, {}};
        std::optional<std::string_view> name;
        std::optional<std::string_view> color;
        std::optional<std::string_view> overlay;
        if (!id || !value || !stringOf(*value, "text", name) || !name || !stringOf(*value, "color", color) ||
            !stringOf(*value, "overlay", overlay) || !argsOf(*value, texts.packed))
            return kErrInvalid;
        runtime::HudBossBar bar;
        bar.op = runtime::HudBossBar::Op::kAdd;
        bar.id = *id;
        bar.name = texts.parse(*name);
        if (const Value* progress = value->find("progress"); progress != nullptr && !progress->isNull()) {
            const double number = progress->asFloat() != nullptr ? *progress->asFloat()
                                  : progress->asInt() != nullptr ? static_cast<double>(*progress->asInt())
                                                                 : std::nan("");
            if (std::isnan(number))
                return kErrInvalid;
            bar.progress = static_cast<float>(std::clamp(number, 0.0, 1.0));
        }
        bar.color = indexOf(kColors, color.value_or("purple"), 5);
        bar.overlay = indexOf(kOverlays, overlay.value_or("progress"), 0);
        return deliver(plugin, player, std::move(bar));
    });
}

void bossbarHide(void* user, Call& call) {
    withPlayer(user, call, [&](WasmPlugin& plugin, const WasmHost::Player& player) -> std::int32_t {
        const auto id = idOf(call, 1);
        if (!id)
            return kErrInvalid;
        runtime::HudBossBar bar;
        bar.op = runtime::HudBossBar::Op::kRemove;
        bar.id = *id;
        return deliver(plugin, player, std::move(bar));
    });
}

void bossbarProgress(void* user, Call& call) {
    withPlayer(user, call, [&](WasmPlugin& plugin, const WasmHost::Player& player) -> std::int32_t {
        const auto id = idOf(call, 1);
        if (!id || std::isnan(call.f64(3)))
            return kErrInvalid;
        runtime::HudBossBar bar;
        bar.op = runtime::HudBossBar::Op::kProgress;
        bar.id = *id;
        bar.progress = static_cast<float>(std::clamp(call.f64(3), 0.0, 1.0));
        return deliver(plugin, player, std::move(bar));
    });
}

void bossbarTitle(void* user, Call& call) {
    withPlayer(user, call, [&](WasmPlugin& plugin, const WasmHost::Player& player) -> std::int32_t {
        const auto id = idOf(call, 1);
        const auto name = text(call, 3);
        const auto args = call.memory().string(call.i32(5), call.i32(6));
        if (!id || !name || !args)
            return kErrInvalid;
        runtime::HudBossBar bar;
        bar.op = runtime::HudBossBar::Op::kName;
        bar.id = *id;
        bar.name = Texts{plugin, std::string{*args}}.parse(*name);
        return deliver(plugin, player, std::move(bar));
    });
}

void bossbarStyle(void* user, Call& call) {
    withPlayer(user, call, [&](WasmPlugin& plugin, const WasmHost::Player& player) -> std::int32_t {
        const auto id = idOf(call, 1);
        const auto color = text(call, 3);
        const auto overlay = text(call, 5);
        if (!id || !color || !overlay)
            return kErrInvalid;
        runtime::HudBossBar bar;
        bar.op = runtime::HudBossBar::Op::kStyle;
        bar.id = *id;
        bar.color = indexOf(kColors, *color, 5);
        bar.overlay = indexOf(kOverlays, *overlay, 0);
        return deliver(plugin, player, std::move(bar));
    });
}

void sidebar(void* user, Call& call) {
    withPlayer(user, call, [&](WasmPlugin& plugin, const WasmHost::Player& player) -> std::int32_t {
        const auto value = spec(call, 1);
        Texts texts{plugin, {}};
        std::optional<std::string_view> title;
        if (!value || !stringOf(*value, "title", title) || !title || !argsOf(*value, texts.packed))
            return kErrInvalid;
        runtime::HudSidebar bar;
        bar.title = texts.parse(*title);
        const Value* lines = value->find("lines");
        if (lines != nullptr && !lines->isNull()) {
            if (lines->asList() == nullptr)
                return kErrInvalid;
            for (const Value& line : *lines->asList()) {
                if (line.asString() == nullptr)
                    return kErrInvalid;
                if (bar.lines.size() < kMaxSidebarLines)
                    bar.lines.push_back(texts.parse(*line.asString()));
            }
        }
        return deliver(plugin, player, std::move(bar));
    });
}

void sidebarClear(void* user, Call& call) {
    withPlayer(user, call, [&](WasmPlugin& plugin, const WasmHost::Player& player) {
        runtime::HudSidebar bar;
        bar.remove = true;
        return deliver(plugin, player, std::move(bar));
    });
}

void tabEntry(void* user, Call& call) {
    withPlayer(user, call, [&](WasmPlugin& plugin, const WasmHost::Player& player) -> std::int32_t {
        const auto id = idOf(call, 1);
        const auto value = spec(call, 3);
        Texts texts{plugin, {}};
        std::optional<std::string_view> name;
        std::optional<std::string_view> label;
        if (!id || !value || !stringOf(*value, "name", name) || !name || !stringOf(*value, "text", label) || !label ||
            !argsOf(*value, texts.packed))
            return kErrInvalid;
        runtime::HudTabEntry entry;
        entry.id = *id;
        entry.name = *name;
        entry.text = texts.parse(*label);
        if (const Value* latency = value->find("latency"); latency != nullptr && !latency->isNull()) {
            if (latency->asInt() == nullptr)
                return kErrInvalid;
            entry.latencyMillis = static_cast<std::int32_t>(std::clamp<std::int64_t>(*latency->asInt(), 0, 10000));
        }
        return deliver(plugin, player, std::move(entry));
    });
}

void tabEntryRemove(void* user, Call& call) {
    withPlayer(user, call, [&](WasmPlugin& plugin, const WasmHost::Player& player) -> std::int32_t {
        const auto id = idOf(call, 1);
        if (!id)
            return kErrInvalid;
        runtime::HudTabEntry entry;
        entry.id = *id;
        entry.remove = true;
        return deliver(plugin, player, std::move(entry));
    });
}

/// One pair of strings, as a map with label and url or a list of two strings.
[[nodiscard]] bool pairOf(const Value& item, std::string_view& label, std::string_view& url) {
    if (const Value::List* list = item.asList()) {
        if (list->size() != 2 || (*list)[0].asString() == nullptr || (*list)[1].asString() == nullptr)
            return false;
        label = *(*list)[0].asString();
        url = *(*list)[1].asString();
        return true;
    }
    const Value* first = item.find("label");
    const Value* second = item.find("url");
    if (first == nullptr || second == nullptr || first->asString() == nullptr || second->asString() == nullptr)
        return false;
    label = *first->asString();
    url = *second->asString();
    return true;
}

void serverLinks(void* user, Call& call) {
    withPlayer(user, call, [&](WasmPlugin& plugin, const WasmHost::Player& player) -> std::int32_t {
        const auto value = readValue(call.memory(), call.i32(1), call.i32(2));
        if (!value || value->asList() == nullptr)
            return kErrInvalid;
        runtime::HudServerLinks links;
        const Texts texts{plugin, {}};
        for (const Value& item : *value->asList()) {
            std::string_view label;
            std::string_view url;
            if (!pairOf(item, label, url))
                return kErrInvalid;
            runtime::HudServerLinks::Link link;
            link.knownType = indexOf(kLinkTypes, label, -1);
            if (link.knownType < 0)
                link.label = texts.parse(label);
            link.url = std::string{url};
            links.links.push_back(std::move(link));
        }
        return deliver(plugin, player, std::move(links));
    });
}

void chatCompletions(void* user, Call& call) {
    withPlayer(user, call, [&](WasmPlugin& plugin, const WasmHost::Player& player) -> std::int32_t {
        const auto action = text(call, 1);
        const auto value = readValue(call.memory(), call.i32(3), call.i32(4));
        if (!action || !value || value->asList() == nullptr)
            return kErrInvalid;
        const std::int32_t kind = indexOf(kCompletionActions, *action, -1);
        if (kind < 0)
            return kErrInvalid;
        runtime::HudChatCompletions completions;
        completions.action = static_cast<runtime::HudChatCompletions::Action>(kind);
        for (const Value& word : *value->asList()) {
            if (word.asString() == nullptr)
                return kErrInvalid;
            completions.entries.push_back(*word.asString());
        }
        return deliver(plugin, player, std::move(completions));
    });
}

} // namespace

void addProxyHudImports(std::vector<Entry>& out) {
    out.push_back({kProxyModule, "hud_clear_title", clearTitle});
    out.push_back({kProxyModule, "hud_tab_list", tabList});
    out.push_back({kProxyModule, "hud_bossbar_show", bossbarShow});
    out.push_back({kProxyModule, "hud_bossbar_hide", bossbarHide});
    out.push_back({kProxyModule, "hud_bossbar_progress", bossbarProgress});
    out.push_back({kProxyModule, "hud_bossbar_title", bossbarTitle});
    out.push_back({kProxyModule, "hud_bossbar_style", bossbarStyle});
    out.push_back({kProxyModule, "hud_sidebar", sidebar});
    out.push_back({kProxyModule, "hud_sidebar_clear", sidebarClear});
    out.push_back({kProxyModule, "hud_tab_entry", tabEntry});
    out.push_back({kProxyModule, "hud_tab_entry_remove", tabEntryRemove});
    out.push_back({kProxyModule, "hud_server_links", serverLinks});
    out.push_back({kProxyModule, "hud_chat_completions", chatCompletions});
}

} // namespace basalt::proxy::plugins::wasmimport

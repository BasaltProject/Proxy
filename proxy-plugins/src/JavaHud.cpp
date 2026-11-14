#include "JavaMarshalling.hpp"

namespace basalt::proxy::plugins::impl {
namespace {

std::vector<std::string_view> split(std::string_view joined, char separator) {
    std::vector<std::string_view> parts;
    for (std::size_t start = 0; start <= joined.size();) {
        const std::size_t end = std::min(joined.find(separator, start), joined.size());
        if (end > start)
            parts.push_back(joined.substr(start, end - start));
        start = end + 1;
    }
    return parts;
}

std::string bytes(JNIEnv* env, jbyteArray data) {
    const jsize size = data == nullptr ? 0 : env->GetArrayLength(data);
    std::string result(static_cast<std::size_t>(size), '\0');
    if (size > 0)
        env->GetByteArrayRegion(data, 0, size, reinterpret_cast<jbyte*>(result.data()));
    return result;
}

} // namespace

std::optional<runtime::PlayerHud> hudAction(JNIEnv* env, jint kind, jstring first, jstring second, jint x, jint y,
                                            jint z, jfloat number, jbyteArray data) {
    const auto component = [&](jstring json) -> std::optional<wire::text::Component> {
        return json == nullptr ? std::nullopt : componentOf(text(env, json));
    };
    runtime::PlayerHud action;
    switch (kind) {
    case 0: {
        runtime::HudTitle title{component(first), component(second), std::nullopt, std::nullopt, false, false};
        if (x >= 0 && y >= 0 && z >= 0)
            title.times = {x, y, z};
        action = std::move(title);
        break;
    }
    case 1:
        action = runtime::HudTitle{std::nullopt, std::nullopt, std::nullopt, std::nullopt, true, x != 0};
        break;
    case 2:
        action = runtime::HudTitle{std::nullopt, std::nullopt, component(first), std::nullopt, false, false};
        break;
    case 3:
        action = runtime::HudTabList{component(first), component(second)};
        break;
    case 4:
    case 5:
    case 6:
    case 7:
    case 8: {
        runtime::HudBossBar bar;
        bar.op = static_cast<runtime::HudBossBar::Op>(kind - 4);
        bar.id = text(env, first);
        if (auto name = component(second))
            bar.name = std::move(*name);
        bar.progress = number;
        bar.color = x;
        bar.overlay = y;
        action = std::move(bar);
        break;
    }
    case 9:
        action = runtime::HudTransfer{text(env, first), x};
        break;
    case 10: {
        const std::string value = bytes(env, data);
        action = runtime::HudStoreCookie{text(env, first), {value.begin(), value.end()}};
        break;
    }
    case 13:
    case 14: {
        runtime::HudTabEntry entry;
        entry.id = text(env, first);
        entry.remove = kind == 14;
        if (!entry.remove) {
            entry.text = component(second);
            entry.latencyMillis = x;
            entry.name = bytes(env, data);
        }
        action = std::move(entry);
        break;
    }
    case 15: {
        runtime::HudDialog dialog;
        dialog.kind = static_cast<runtime::HudDialog::Kind>(std::clamp(x, 0, 3));
        dialog.canCloseWithEscape = y != 0;
        if (auto title = component(first))
            dialog.title = std::move(*title);
        const std::string body = text(env, second);
        for (const auto line : split(body, '\n'))
            if (auto parsed = componentOf(line); parsed && dialog.body.size() < 32)
                dialog.body.push_back(std::move(*parsed));
        const std::string buttons = bytes(env, data);
        for (const auto row : split(buttons, '\n')) {
            const std::size_t tab = row.find('\t');
            if (tab == std::string_view::npos || dialog.buttons.size() >= 16)
                continue;
            if (auto label = componentOf(row.substr(0, tab)))
                dialog.buttons.push_back({std::move(*label), std::string{row.substr(tab + 1)}});
        }
        action = std::move(dialog);
        break;
    }
    case 16: {
        runtime::HudServerLinks links;
        const std::string rows = bytes(env, data);
        for (const auto row : split(rows, '\n')) {
            const auto cells = split(row, '\t');
            if (cells.size() != 3)
                continue;
            runtime::HudServerLinks::Link link;
            link.knownType = cells[0] == "-" ? -1 : std::clamp(std::atoi(std::string{cells[0]}.c_str()), 0, 9);
            if (link.knownType < 0) {
                auto label = componentOf(cells[1]);
                if (!label)
                    continue;
                link.label = std::move(*label);
            }
            link.url = std::string{cells[2]};
            links.links.push_back(std::move(link));
        }
        action = std::move(links);
        break;
    }
    case 17: {
        runtime::HudChatCompletions completions;
        completions.action = static_cast<runtime::HudChatCompletions::Action>(std::clamp(x, 0, 2));
        const std::string entries = bytes(env, data);
        for (const auto entry : split(entries, '\n'))
            completions.entries.emplace_back(entry);
        action = std::move(completions);
        break;
    }
    case 18:
    case 19:
    case 20: {
        runtime::HudResourcePack pack;
        pack.op = static_cast<runtime::HudResourcePack::Op>(kind - 18);
        const auto id = wire::common::Uuid::parse(text(env, first));
        if (pack.op != runtime::HudResourcePack::Op::kClear && !id)
            return std::nullopt;
        pack.id = id.value_or(wire::common::Uuid{});
        if (pack.op == runtime::HudResourcePack::Op::kPush) {
            const std::string fields = bytes(env, data);
            const std::size_t newline = fields.find('\n');
            pack.url = fields.substr(0, newline);
            pack.hash = newline == std::string::npos ? std::string{} : fields.substr(newline + 1);
            pack.prompt = component(second);
            pack.required = x != 0;
        }
        action = std::move(pack);
        break;
    }
    default:
        return std::nullopt;
    }
    return action;
}

std::vector<std::string> settingsFields(const runtime::ClientSettings& settings) {
    return {settings.locale,
            std::to_string(settings.viewDistance),
            std::to_string(settings.chatMode),
            settings.chatColors ? "1" : "0",
            std::to_string(settings.skinParts),
            std::to_string(settings.mainHand),
            settings.textFiltering ? "1" : "0",
            settings.serverListing ? "1" : "0",
            std::to_string(settings.particles)};
}

} // namespace basalt::proxy::plugins::impl

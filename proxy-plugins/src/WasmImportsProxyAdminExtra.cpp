#include "WasmAdminState.hpp"
#include "WasmImports.hpp"
#include "WasmValues.hpp"

#include <algorithm>

namespace basalt::proxy::plugins::wasmimport {
namespace {

using namespace wire::wasm;
using wire::permissions::Context;
using wire::permissions::ContextEntry;
using wire::permissions::Subject;
using wire::permissions::Tristate;

constexpr std::size_t kMaxAnswerText = 4096;

/// Who a question is about and the context it carries, owning the text the views of Subject and Context point into.
struct Asked {
    std::string name;
    std::string uuid;
    std::string server;
    std::string world;
    bool console = false;
    std::vector<std::pair<std::string, std::string>> extra;
    std::vector<ContextEntry> entries;

    [[nodiscard]] Subject subject() const { return console ? Subject::console() : Subject::player(uuid, name); }

    [[nodiscard]] Context context() {
        entries.clear();
        for (const auto& [key, value] : extra)
            entries.push_back({key, value});
        return Context{server, world, entries};
    }
};

/// The ABI result to answer, or kOk with `asked` filled; the context value is optional (length 0 for none).
[[nodiscard]] std::int32_t askedOf(WasmHost& wasm, Call& call, std::string_view who, std::size_t contextIndex,
                                   Asked& asked) {
    asked.console = who.empty();
    asked.name = who.empty() ? "console" : std::string{who};
    if (!asked.console) {
        if (const WasmHost::Player* player = wasm.playerOf(wasm.handleOf(who))) {
            asked.name = player->name;
            asked.uuid = player->uuid;
            asked.server = player->server;
        }
    }
    if (call.i32(contextIndex + 1) == 0)
        return kOk;
    const auto value = readValue(call.memory(), call.i32(contextIndex), call.i32(contextIndex + 1));
    const Value::Map* map = value ? value->asMap() : nullptr;
    if (map == nullptr)
        return kErrInvalid;
    for (const MapEntry& entry : *map) {
        const std::string* text = entry.value.asString();
        if (text == nullptr)
            return kErrInvalid;
        if (entry.key == "server")
            asked.server = *text;
        else if (entry.key == "world")
            asked.world = *text;
        else
            asked.extra.emplace_back(entry.key, *text);
    }
    return kOk;
}

void permissionCheck(void* user, Call& call) {
    WasmHost& wasm = *self(user).host;
    const auto who = text(call, 0);
    const auto node = text(call, 2);
    if (!who || !node || node->empty())
        return call.ret(kErrInvalid);
    Asked asked;
    if (const std::int32_t status = askedOf(wasm, call, *who, 4, asked); status != kOk)
        return call.ret(status);
    auto* chain = wasm.hooks().permissions;
    const Tristate answer =
        chain != nullptr ? chain->check(asked.subject(), *node, asked.context()) : Tristate::kUndefined;
    call.ret(answer == Tristate::kUndefined ? kErrAbsent : (answer == Tristate::kTrue ? 1 : 0));
}

void permissionMeta(void* user, Call& call) {
    WasmHost& wasm = *self(user).host;
    const auto who = text(call, 0);
    const auto key = text(call, 2);
    if (!who || !key || key->empty())
        return call.ret(kErrInvalid);
    Asked asked;
    if (const std::int32_t status = askedOf(wasm, call, *who, 4, asked); status != kOk)
        return call.ret(status);
    auto* chain = wasm.hooks().permissions;
    const auto value = chain != nullptr ? chain->meta(asked.subject(), *key, asked.context()) : std::nullopt;
    call.ret(value ? call.memory().put(call.i32(6), call.i32(7), *value) : kErrAbsent);
}

[[nodiscard]] Value requestValue(bool meta, const Subject& subject, std::string_view what, const Context& context) {
    Value::Map extra;
    extra.push_back({"server", Value{std::string{context.server}}});
    extra.push_back({"world", Value{std::string{context.world}}});
    for (const ContextEntry& entry : context.extra)
        if (entry.key != "server" && entry.key != "world")
            extra.push_back({std::string{entry.key}, Value{std::string{entry.value}}});
    Value::Map map;
    map.push_back({"kind", Value{meta ? "meta" : "check"}});
    map.push_back({"name", Value{std::string{subject.name}}});
    map.push_back({"uuid", Value{std::string{subject.uuid}}});
    map.push_back({"console", Value{subject.isConsole()}});
    map.push_back({"text", Value{std::string{what}}});
    map.push_back({"context", Value{std::move(extra)}});
    return Value{std::move(map)};
}

/// On the plugin thread: runs the provider's callback for one question and wakes the asker.
void runAsk(const std::weak_ptr<ProviderSlot>& weak, const std::shared_ptr<PermissionAsk>& ask,
            const std::shared_ptr<const Value>& value) {
    bool wanted = false;
    {
        const std::lock_guard lock{ask->mutex};
        wanted = ask->open;
    }
    if (const auto slot = weak.lock(); slot && wanted) {
        WasmPlugin& plugin = *slot->plugin;
        AdminState& state = adminState(plugin);
        state.ask = ask;
        state.askValue = value.get();
        WasmCall call;
        call.value = value.get();
        plugin.host->dispatch(plugin, ask->meta ? slot->metaToken : slot->checkToken, &call, "permission provider",
                              kWasmCallBudget);
        state.ask.reset();
        state.askValue = nullptr;
    }
    {
        const std::lock_guard lock{ask->mutex};
        ask->finished = true;
    }
    ask->done.notify_all();
}

/// Called from any thread by the chain: queues the question to the plugin thread and waits for the answer.
[[nodiscard]] std::shared_ptr<PermissionAsk> askPlugin(PluginHost& host, const std::weak_ptr<ProviderSlot>& weak,
                                                       bool meta, const Subject& subject, std::string_view what,
                                                       const Context& context) {
    if (weak.expired())
        return nullptr;
    auto ask = std::make_shared<PermissionAsk>();
    ask->meta = meta;
    const auto value = std::make_shared<const Value>(requestValue(meta, subject, what, context));
    if (host.onPluginThread()) {
        const auto slot = weak.lock();
        if (!slot || slot->plugin->host->busy())
            return nullptr;
        runAsk(weak, ask, value);
        return ask;
    }
    if (!host.runOnPluginThread([weak, ask, value] { runAsk(weak, ask, value); }, true))
        return nullptr;
    std::unique_lock lock{ask->mutex};
    if (!ask->done.wait_for(lock, kProviderDeadline, [&ask] { return ask->finished; })) {
        ask->open = false;
        return nullptr;
    }
    return ask;
}

void permissionProvide(void* user, Call& call) {
    WasmPlugin& plugin = self(user);
    auto* chain = plugin.host->hooks().permissions;
    if (chain == nullptr)
        return call.ret(kErrAbsent);
    const std::int32_t checkToken = call.i32(0);
    const std::int32_t metaToken = call.i32(1);
    if ((checkToken <= 0 && metaToken <= 0) || call.i32(2) < 0)
        return call.ret(kErrInvalid);
    auto slot = std::make_shared<ProviderSlot>();
    slot->plugin = &plugin;
    slot->checkToken = std::max(checkToken, 0);
    slot->metaToken = std::max(metaToken, 0);
    PluginHost* host = &plugin.host->host();
    const std::weak_ptr<ProviderSlot> weak = slot;
    wire::permissions::Provider provider{.id = "wasm:" + plugin.id,
                                         .priority =
                                             call.i32(2) == 0 ? wire::permissions::kPriorityPlugin : call.i32(2),
                                         .check = {},
                                         .meta = {}};
    if (slot->checkToken > 0)
        provider.check = [host, weak](const wire::permissions::Query& query) {
            const auto ask = askPlugin(*host, weak, false, query.subject(), query.node(), query.context());
            return ask ? ask->answer : Tristate::kUndefined;
        };
    if (slot->metaToken > 0)
        provider.meta = [host, weak](const Subject& subject, std::string_view key,
                                     const Context& context) -> std::optional<std::string> {
            const auto ask = askPlugin(*host, weak, true, subject, key, context);
            return ask ? ask->text : std::nullopt;
        };
    AdminState& state = adminState(plugin);
    const std::int32_t handle = state.providers.add(slot);
    if (handle == 0)
        return call.ret(kErrInvalid);
    slot->handle = chain->provide(std::move(provider));
    call.ret(handle);
}

void permissionUnprovide(void* user, Call& call) {
    AdminState& state = adminState(self(user));
    std::shared_ptr<ProviderSlot>* slot = nullptr;
    if (const std::int32_t status = state.providers.find(call.i32(0), slot); status != kOk)
        return call.ret(status);
    (*slot)->handle.remove();
    static_cast<void>(state.providers.remove(call.i32(0)));
    call.ret(kOk);
}

/// The live provider question of a ctx, or the ABI result to answer.
[[nodiscard]] std::int32_t askOf(WasmPlugin& plugin, std::int32_t ctx, std::shared_ptr<PermissionAsk>& ask) {
    const WasmCall* live = plugin.host->live(ctx);
    if (live == nullptr)
        return ctx >= 1 && static_cast<std::uint32_t>(ctx) < plugin.host->nextCall_ ? kErrStale : kErrInvalid;
    AdminState& state = adminState(plugin);
    if (!state.ask || live->value != state.askValue)
        return kErrInvalid;
    ask = state.ask;
    return kOk;
}

void permissionRequest(void* user, Call& call) {
    WasmPlugin& plugin = self(user);
    std::shared_ptr<PermissionAsk> ask;
    if (const std::int32_t status = askOf(plugin, call.i32(0), ask); status != kOk)
        return call.ret(status);
    {
        const std::lock_guard lock{ask->mutex};
        if (!ask->open)
            return call.ret(kErrStale);
    }
    call.ret(putValue(call.memory(), call.i32(1), call.i32(2), *adminState(plugin).askValue));
}

void answer(void* user, Call& call, bool meta) {
    WasmPlugin& plugin = self(user);
    std::shared_ptr<PermissionAsk> ask;
    if (const std::int32_t status = askOf(plugin, call.i32(0), ask); status != kOk)
        return call.ret(status);
    std::optional<std::string> line;
    if (meta && call.i32(2) >= 0) {
        const auto text = call.memory().string(call.i32(1), call.i32(2));
        if (!text)
            return call.ret(kErrInvalid);
        line = std::string{text->substr(0, kMaxAnswerText)};
    }
    const std::lock_guard lock{ask->mutex};
    if (!ask->open)
        return call.ret(kErrStale);
    if (ask->meta != meta)
        return call.ret(kErrInvalid);
    if (meta)
        ask->text = std::move(line);
    else
        ask->answer = call.i32(1) < 0 ? Tristate::kUndefined : wire::permissions::tristateOf(call.i32(1) != 0);
    call.ret(kOk);
}

void permissionAnswer(void* user, Call& call) { answer(user, call, false); }

void permissionAnswerText(void* user, Call& call) { answer(user, call, true); }

void permissionChanged(void* user, Call& call) {
    auto* chain = self(user).host->hooks().permissions;
    if (chain == nullptr)
        return call.ret(kErrAbsent);
    chain->changed();
    call.ret(kOk);
}

void permissionOverride(void* user, Call& call) {
    WasmHost& wasm = *self(user).host;
    const auto name = text(call, 0);
    const auto node = text(call, 2);
    const std::int32_t value = call.i32(4);
    if (!name || !node || name->empty() || node->empty() || value < -1 || value > 1 || !wasm.hooks().setPermission)
        return call.ret(kErrInvalid);
    wasm.hooks().setPermission(*name, *node, value);
    call.ret(kOk);
}

void permissionList(void* user, Call& call) {
    WasmHost& wasm = *self(user).host;
    const auto name = text(call, 0);
    if (!name || name->empty() || !wasm.hooks().permissionList)
        return call.ret(kErrInvalid);
    std::vector<std::string> nodes = wasm.hooks().permissionList(*name);
    std::ranges::sort(nodes);
    Value::List list;
    list.reserve(nodes.size());
    for (std::string& node : nodes)
        list.emplace_back(std::move(node));
    call.ret(putValue(call.memory(), call.i32(2), call.i32(3), Value{std::move(list)}));
}

void groupGet(void* user, Call& call) {
    WasmHost& wasm = *self(user).host;
    const auto name = text(call, 0);
    if (!name || name->empty() || !wasm.hooks().groupOf)
        return call.ret(kErrInvalid);
    call.ret(call.memory().put(call.i32(2), call.i32(3), wasm.hooks().groupOf(*name)));
}

void groupSet(void* user, Call& call) {
    WasmHost& wasm = *self(user).host;
    const auto name = text(call, 0);
    const auto group = text(call, 2);
    if (!name || !group || name->empty() || !wasm.hooks().setGroup)
        return call.ret(kErrInvalid);
    call.ret(wasm.hooks().setGroup(*name, *group) ? kOk : kErrInvalid);
}

constexpr std::array kAdminExtra = std::to_array<Entry>({
    {kProxyModule, "permission_check", permissionCheck},
    {kProxyModule, "permission_meta", permissionMeta},
    {kProxyModule, "permission_provide", permissionProvide},
    {kProxyModule, "permission_unprovide", permissionUnprovide},
    {kProxyModule, "permission_request", permissionRequest},
    {kProxyModule, "permission_answer", permissionAnswer},
    {kProxyModule, "permission_answer_text", permissionAnswerText},
    {kProxyModule, "permission_changed", permissionChanged},
    {kProxyModule, "permission_override", permissionOverride},
    {kProxyModule, "permission_list", permissionList},
    {kProxyModule, "group_get", groupGet},
    {kProxyModule, "group_set", groupSet},
});

} // namespace

void addProxyAdminExtraImports(std::vector<Entry>& out) {
    out.insert(out.end(), kAdminExtra.begin(), kAdminExtra.end());
}

} // namespace basalt::proxy::plugins::wasmimport

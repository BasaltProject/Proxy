#pragma once

#include "JavaMarshalling.hpp"

#include <basalt/wire/permissions/PermissionChain.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace basalt::proxy::plugins {

/// A facade's seat in the permission chain: the checks its plugins ask and the provider it may register.
class JavaPermissions {
public:
    /// The facade's answer for a player (empty for the console): -1 undefined, 0 or 1; rest is the chain below, or -1.
    using Ask = std::function<int(const std::string& player, const std::string& node, int rest)>;
    /// A text option (prefix, suffix or any key) for a player, empty name for the console; nothing when it has none.
    using AskMeta = std::function<std::optional<std::string>(const std::string& player, const std::string& key)>;
    /// The uuid and backend of an online player.
    using Locate = std::function<std::optional<std::pair<std::string, std::string>>(const std::string& player)>;

    JavaPermissions(const PluginHooks& hooks, std::string id, std::int32_t priority, Ask ask, AskMeta askMeta,
                    Locate locate);
    ~JavaPermissions();
    JavaPermissions(const JavaPermissions&) = delete;
    JavaPermissions& operator=(const JavaPermissions&) = delete;

    [[nodiscard]] const PluginHooks& hooks() const noexcept { return *hooks_; }

    /// What the whole chain answers for a player, or the console when the name is empty: -1, 0 or 1.
    [[nodiscard]] int value(const std::string& player, const std::string& node) const;

    /// Takes the provider out for good and waits for the checks running in it; before the Java machine stops.
    void retire();

    /// Puts the facade's provider in the chain or takes it out; withRest hands it the answer of the chain below.
    void provide(bool active, bool withRest);

    /// Puts the facade's meta provider in the chain or takes it out.
    void provideMeta(bool active);

private:
    struct Seat;

    const PluginHooks* hooks_;
    std::string id_;
    std::int32_t priority_;
    Locate locate_;
    std::shared_ptr<Seat> seat_;
};

/// The permissions of the facade in a slot, or null when none is running there.
[[nodiscard]] JavaPermissions* javaPermissionsOf(int slot);

namespace javaPermissionNatives {

using impl::text;

template <int Slot>
jboolean permission(JNIEnv* env, jclass, jstring player, jstring node) {
    const auto* seat = javaPermissionsOf(Slot);
    return seat != nullptr && seat->value(text(env, player), text(env, node)) == 1 ? JNI_TRUE : JNI_FALSE;
}

template <int Slot>
jint permissionValue(JNIEnv* env, jclass, jstring player, jstring node) {
    const auto* seat = javaPermissionsOf(Slot);
    return seat != nullptr ? seat->value(text(env, player), text(env, node)) : -1;
}

template <int Slot>
void permissionProvider(JNIEnv*, jclass, jboolean active, jboolean withRest) {
    if (auto* seat = javaPermissionsOf(Slot))
        seat->provide(active == JNI_TRUE, withRest == JNI_TRUE);
}

template <int Slot>
void permissionMeta(JNIEnv*, jclass, jboolean active) {
    if (auto* seat = javaPermissionsOf(Slot))
        seat->provideMeta(active == JNI_TRUE);
}

template <int Slot>
jint permissionOverride(JNIEnv* env, jclass, jstring player, jstring node) {
    const auto* seat = javaPermissionsOf(Slot);
    return seat != nullptr && seat->hooks().permissionOverride
               ? seat->hooks().permissionOverride(text(env, player), text(env, node))
               : -1;
}

template <int Slot>
jobjectArray permissionList(JNIEnv* env, jclass, jstring player) {
    const auto* seat = javaPermissionsOf(Slot);
    const std::vector<std::string> nodes = seat != nullptr && seat->hooks().permissionList
                                               ? seat->hooks().permissionList(text(env, player))
                                               : std::vector<std::string>{};
    return impl::stringArray(env, nodes);
}

template <int Slot>
jstring group(JNIEnv* env, jclass, jstring player) {
    const auto* seat = javaPermissionsOf(Slot);
    const std::string name = seat != nullptr && seat->hooks().groupOf ? seat->hooks().groupOf(text(env, player)) : "";
    return env->NewStringUTF(name.c_str());
}

template <int Slot>
jboolean setGroup(JNIEnv* env, jclass, jstring player, jstring name) {
    const auto* seat = javaPermissionsOf(Slot);
    return seat != nullptr && seat->hooks().setGroup && seat->hooks().setGroup(text(env, player), text(env, name))
               ? JNI_TRUE
               : JNI_FALSE;
}

template <int Slot>
void setPermission(JNIEnv* env, jclass, jstring player, jstring node, jint value) {
    const auto* seat = javaPermissionsOf(Slot);
    if (seat != nullptr && seat->hooks().setPermission)
        seat->hooks().setPermission(text(env, player), text(env, node), value);
}

} // namespace javaPermissionNatives
} // namespace basalt::proxy::plugins

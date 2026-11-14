#pragma once

#include "PluginInternal.hpp"

#include <basalt/proxy/plugins/JarDetector.hpp>
#include <basalt/proxy/plugins/JavaAdapter.hpp>
#include <basalt/wire/common/json/Json.hpp>
#include <basalt/wire/shim/ShimHost.hpp>
#include <basalt/wire/text/ComponentJson.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>

namespace basalt::proxy::plugins {
namespace impl {

using runtime::DecisionKind;
using wire::shim::ShimHost;

struct PlayerRow {
    std::string name;
    std::string uuid;
    std::string server;
    std::string address;
    std::uint16_t port = 0;
    std::int64_t ping = -1;
};

/// A facade's decision answer: deny, reason, redirect, text, max, online, then for a ping the JSON flags ("r", "t"),
/// and "=" prefixed version name, protocol, newline separated sample and icon data URI, each empty when kept.
inline std::optional<runtime::DecisionResult> decisionOf(const std::vector<std::string>& answer, bool ping) {
    if (answer.size() < 6)
        return std::nullopt;
    runtime::DecisionResult result;
    result.deny = answer[0] == "1";
    result.reason = answer[1];
    result.redirect = answer[2];
    result.text = answer[3];
    if (answer.size() > 6) {
        result.reasonJson = answer[6].contains('r');
        result.textJson = answer[6].contains('t');
    }
    if (!ping)
        return result;
    if (const auto max = std::stoll(answer[4]); max >= 0)
        result.maxPlayers = static_cast<std::int32_t>(max);
    if (const auto online = std::stoll(answer[5]); online >= 0)
        result.online = static_cast<std::int32_t>(online);
    const auto given = [&answer](std::size_t at) -> std::optional<std::string_view> {
        if (answer.size() <= at || !answer[at].starts_with('='))
            return std::nullopt;
        return std::string_view{answer[at]}.substr(1);
    };
    if (const auto name = given(7))
        result.versionName = std::string{name->substr(0, kMaxSampleLine)};
    if (answer.size() > 8)
        if (const auto protocol = std::stoll(answer[8]); protocol >= 0 && protocol <= 0x7fffffff)
            result.versionProtocol = static_cast<std::int32_t>(protocol);
    if (const auto lines = given(9)) {
        std::vector<std::string> sample;
        std::string_view rest = *lines;
        while (!rest.empty() && sample.size() < kMaxSampleLines) {
            const auto end = rest.find('\n');
            sample.emplace_back(rest.substr(0, std::min(end, kMaxSampleLine)));
            rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
        }
        result.sample = std::move(sample);
    }
    if (const auto icon = given(10))
        result.favicon = faviconOf(*icon);
    return result;
}

inline std::string text(JNIEnv* env, jstring value) {
    if (value == nullptr)
        return {};
    const char* chars = env->GetStringUTFChars(value, nullptr);
    std::string result{chars};
    env->ReleaseStringUTFChars(value, chars);
    return result;
}

inline jobjectArray stringArray(JNIEnv* env, const std::vector<std::string>& values) {
    jclass string = env->FindClass("java/lang/String");
    jobjectArray array = env->NewObjectArray(static_cast<jsize>(values.size()), string, nullptr);
    for (std::size_t i = 0; i < values.size(); ++i) {
        jstring item = env->NewStringUTF(values[i].c_str());
        env->SetObjectArrayElement(array, static_cast<jsize>(i), item);
        env->DeleteLocalRef(item);
    }
    env->DeleteLocalRef(string);
    return array;
}

inline std::vector<std::string> strings(JNIEnv* env, jobjectArray array) {
    std::vector<std::string> values;
    if (array == nullptr)
        return values;
    const jsize size = env->GetArrayLength(array);
    for (jsize i = 0; i < size; ++i) {
        auto* item = static_cast<jstring>(env->GetObjectArrayElement(array, i));
        values.push_back(text(env, item));
        env->DeleteLocalRef(item);
    }
    return values;
}

inline std::optional<wire::text::Component> componentOf(std::string_view json) {
    const auto parsed = wire::common::parseJson(json);
    if (!parsed)
        return std::nullopt;
    return wire::text::componentJson::deserialize(*parsed);
}

/// What a facade's `hud` call asks for, read from its packed arguments; empty for an unknown kind.
[[nodiscard]] std::optional<runtime::PlayerHud> hudAction(JNIEnv* env, jint kind, jstring first, jstring second, jint x,
                                                          jint y, jint z, jfloat number, jbyteArray data);

/// Client settings in the order the facades read them, starting with the locale.
[[nodiscard]] std::vector<std::string> settingsFields(const runtime::ClientSettings& settings);

inline constexpr std::size_t kMaxSettingsKept = 1024;
inline constexpr std::size_t kMaxPendingPings = 64;

} // namespace impl

using namespace impl;

} // namespace basalt::proxy::plugins

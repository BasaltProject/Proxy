#pragma once

#include <basalt/proxy/plugins/JarDetector.hpp>
#include <basalt/proxy/plugins/JavaAdapter.hpp>
#include <basalt/wire/common/json/Json.hpp>
#include <basalt/wire/shim/ShimHost.hpp>
#include <basalt/wire/text/ComponentJson.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
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

} // namespace impl

using namespace impl;

} // namespace basalt::proxy::plugins

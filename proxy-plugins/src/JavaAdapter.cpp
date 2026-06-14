#include <basalt/proxy/plugins/JarDetector.hpp>
#include <basalt/proxy/plugins/JavaAdapter.hpp>
#include <basalt/wire/common/json/Json.hpp>
#include <basalt/wire/shim/ShimHost.hpp>
#include <basalt/wire/text/ComponentJson.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>

namespace basalt::proxy::plugins {
namespace {

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

std::string text(JNIEnv* env, jstring value) {
    if (value == nullptr)
        return {};
    const char* chars = env->GetStringUTFChars(value, nullptr);
    std::string result{chars};
    env->ReleaseStringUTFChars(value, chars);
    return result;
}

jobjectArray stringArray(JNIEnv* env, const std::vector<std::string>& values) {
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

std::vector<std::string> strings(JNIEnv* env, jobjectArray array) {
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

std::optional<wire::text::Component> componentOf(std::string_view json) {
    const auto parsed = wire::common::parseJson(json);
    if (!parsed)
        return std::nullopt;
    return wire::text::componentJson::deserialize(*parsed);
}

} // namespace

class JavaAdapter;
static JavaAdapter* gAdapters[2] = {nullptr, nullptr};

class JavaAdapter final : public ForeignAdapter {
public:
    JavaAdapter(std::shared_ptr<ShimHost> shim, PluginHooks hooks, JavaSpec spec)
        : shim_(std::move(shim)), hooks_(std::move(hooks)), spec_(std::move(spec)) {}

    ~JavaAdapter() override {
        if (JNIEnv* env = shim_->env()) {
            call(env, "shutdown", "()V",
                 [&](jclass facade, jmethodID method) { env->CallStaticVoidMethod(facade, method); });
        }
        gAdapters[spec_.slot] = nullptr;
        const auto machine = std::move(shim_);
        if (machine.use_count() == 1) {
            machine->beginShutdown();
            machine->destroy();
        }
    }

    [[nodiscard]] std::expected<void, std::string> boot(const std::string& folder) {
        JNIEnv* env = shim_->env();
        if (env == nullptr)
            return std::unexpected("the Java machine has no environment for this thread");
        jclass natives = env->FindClass(spec_.natives.c_str());
        if (natives == nullptr) {
            env->ExceptionClear();
            return std::unexpected("the Java facade is not in the runtime folder");
        }
        const auto methods = spec_.slot == 0 ? methodTable<0>() : methodTable<1>();

        const jint registered = env->RegisterNatives(natives, methods.data(), static_cast<jint>(methods.size()));
        env->DeleteLocalRef(natives);
        if (registered != JNI_OK)
            return std::unexpected("could not register the native methods of the Java facade");
        gAdapters[spec_.slot] = this;
        std::string failure;
        call(env, "boot", "(Ljava/lang/String;)V", [&](jclass facade, jmethodID method) {
            jstring argument = env->NewStringUTF(folder.c_str());
            env->CallStaticVoidMethod(facade, method, argument);
            env->DeleteLocalRef(argument);
        });
        return {};
    }

    [[nodiscard]] std::string_view label() const override { return spec_.label; }

    [[nodiscard]] std::vector<ForeignPlugin> plugins() const override {
        std::vector<ForeignPlugin> found;
        JNIEnv* env = shim_->env();
        if (env == nullptr)
            return found;
        const auto rows = callStrings(env, "plugins");
        for (const std::string& row : rows) {
            const auto tab = row.find('\t');
            found.push_back({row.substr(0, tab), tab == std::string::npos ? std::string{} : row.substr(tab + 1)});
        }
        return found;
    }

    [[nodiscard]] std::vector<std::string> commands() const override {
        JNIEnv* env = shim_->env();
        return env == nullptr ? std::vector<std::string>{} : callStrings(env, "commands");
    }

    [[nodiscard]] bool decides(DecisionKind) const override { return true; }

    [[nodiscard]] std::optional<runtime::DecisionResult> decide(const runtime::DecisionRequest& request) override {
        JNIEnv* env = shim_->env();
        if (env == nullptr)
            return std::nullopt;
        const char* kind = "login";
        switch (request.kind) {
        case DecisionKind::kLogin:
            kind = "login";
            break;
        case DecisionKind::kPreConnect:
            kind = "preconnect";
            break;
        case DecisionKind::kKick:
            kind = "kick";
            break;
        case DecisionKind::kChat:
            kind = "chat";
            break;
        case DecisionKind::kCommand:
            kind = "command";
            break;
        case DecisionKind::kPing:
            kind = "ping";
            break;
        }
        const std::vector<std::string> fields{request.player,
                                              request.uuid,
                                              request.from,
                                              request.address,
                                              std::to_string(request.port),
                                              request.host,
                                              request.kind == DecisionKind::kPreConnect ? request.backend
                                                                                        : request.text};
        std::vector<std::string> answer;
        call(env, "decide", "(Ljava/lang/String;[Ljava/lang/String;)[Ljava/lang/String;",
             [&](jclass facade, jmethodID method) {
                 jstring name = env->NewStringUTF(kind);
                 jobjectArray array = stringArray(env, fields);
                 auto* result = static_cast<jobjectArray>(env->CallStaticObjectMethod(facade, method, name, array));
                 if (!env->ExceptionCheck())
                     answer = strings(env, result);
                 env->DeleteLocalRef(name);
                 env->DeleteLocalRef(array);
                 if (result != nullptr)
                     env->DeleteLocalRef(result);
             });
        if (answer.size() < 6)
            return std::nullopt;
        runtime::DecisionResult result;
        result.deny = answer[0] == "1";
        result.reason = answer[1];
        result.redirect = answer[2];
        result.text = answer[3];
        if (request.kind == DecisionKind::kPing) {
            if (const auto max = std::stoll(answer[4]); max >= 0)
                result.maxPlayers = static_cast<std::int32_t>(max);
            if (const auto online = std::stoll(answer[5]); online >= 0)
                result.online = static_cast<std::int32_t>(online);
        }
        return result;
    }

    void notify(const runtime::PlayerEvent& event) override {
        using Kind = runtime::PlayerEventKind;
        {
            std::lock_guard lock{mutex_};
            if (event.kind == Kind::kLatency) {
                if (const auto known = players_.find(event.player); known != players_.end())
                    known->second.ping = event.value;
            } else if (event.kind == Kind::kJoin || event.kind == Kind::kSwitch) {
                auto& row = players_[event.player];
                row.name = event.player;
                if (!event.uuid.empty())
                    row.uuid = event.uuid;
                row.server = event.backend;
                if (!event.address.empty()) {
                    row.address = event.address;
                    row.port = event.port;
                }
            }
        }
        const char* kind = nullptr;
        if (event.kind == Kind::kJoin)
            kind = "join";
        else if (event.kind == Kind::kSwitch)
            kind = "switch";
        else if (event.kind == Kind::kLeave)
            kind = "quit";
        JNIEnv* env = shim_->env();
        if (kind != nullptr && env != nullptr) {
            const std::vector<std::string> fields{
                event.player, event.uuid, event.backend, event.address, std::to_string(event.port), {}, event.backend};
            call(env, "notifyPlayer", "(Ljava/lang/String;[Ljava/lang/String;)V", [&](jclass facade, jmethodID method) {
                jstring name = env->NewStringUTF(kind);
                jobjectArray array = stringArray(env, fields);
                env->CallStaticVoidMethod(facade, method, name, array);
                env->DeleteLocalRef(name);
                env->DeleteLocalRef(array);
            });
        }
        if (event.kind == Kind::kLeave) {
            std::lock_guard lock{mutex_};
            players_.erase(event.player);
        }
    }

    [[nodiscard]] bool listens(std::string_view channel) const override {
        std::lock_guard lock{mutex_};
        return channels_.contains(channel);
    }

    [[nodiscard]] bool message(std::string_view player, std::string_view channel, std::span<const std::uint8_t> data,
                               bool fromBackend) override {
        JNIEnv* env = shim_->env();
        if (env == nullptr)
            return false;
        bool cancelled = false;
        call(env, "message", "(Ljava/lang/String;Ljava/lang/String;[BZ)Z", [&](jclass facade, jmethodID method) {
            jstring who = env->NewStringUTF(std::string{player}.c_str());
            jstring tag = env->NewStringUTF(std::string{channel}.c_str());
            jbyteArray bytes = env->NewByteArray(static_cast<jsize>(data.size()));
            env->SetByteArrayRegion(bytes, 0, static_cast<jsize>(data.size()),
                                    reinterpret_cast<const jbyte*>(data.data()));
            cancelled = env->CallStaticBooleanMethod(facade, method, who, tag, bytes,
                                                     fromBackend ? JNI_TRUE : JNI_FALSE) == JNI_TRUE;
            env->DeleteLocalRef(who);
            env->DeleteLocalRef(tag);
            env->DeleteLocalRef(bytes);
        });
        return cancelled;
    }

    [[nodiscard]] bool console(std::string_view line) override {
        JNIEnv* env = shim_->env();
        if (env == nullptr)
            return false;
        bool handled = false;
        call(env, "console", "(Ljava/lang/String;)Z", [&](jclass facade, jmethodID method) {
            jstring argument = env->NewStringUTF(std::string{line}.c_str());
            handled = env->CallStaticBooleanMethod(facade, method, argument) == JNI_TRUE;
            env->DeleteLocalRef(argument);
        });
        return handled;
    }

    template <int Slot>
    static std::array<JNINativeMethod, 10> methodTable() {
        return {{
            {const_cast<char*>("log"), const_cast<char*>("(Ljava/lang/String;)V"),
             reinterpret_cast<void*>(nativeLog<Slot>)},
            {const_cast<char*>("connect"), const_cast<char*>("(Ljava/lang/String;Ljava/lang/String;)Z"),
             reinterpret_cast<void*>(nativeConnect<Slot>)},
            {const_cast<char*>("kick"), const_cast<char*>("(Ljava/lang/String;Ljava/lang/String;)V"),
             reinterpret_cast<void*>(nativeKick<Slot>)},
            {const_cast<char*>("tell"), const_cast<char*>("(Ljava/lang/String;Ljava/lang/String;)V"),
             reinterpret_cast<void*>(nativeTell<Slot>)},
            {const_cast<char*>("send"), const_cast<char*>("(Ljava/lang/String;Ljava/lang/String;[BZ)Z"),
             reinterpret_cast<void*>(nativeSend<Slot>)},
            {const_cast<char*>("servers"), const_cast<char*>("()[Ljava/lang/String;"),
             reinterpret_cast<void*>(nativeServers<Slot>)},
            {const_cast<char*>("serverAddress"), const_cast<char*>("(Ljava/lang/String;)Ljava/lang/String;"),
             reinterpret_cast<void*>(nativeServerAddress<Slot>)},
            {const_cast<char*>("players"), const_cast<char*>("()[[Ljava/lang/String;"),
             reinterpret_cast<void*>(nativePlayers<Slot>)},
            {const_cast<char*>("config"), const_cast<char*>("()[Ljava/lang/String;"),
             reinterpret_cast<void*>(nativeConfig<Slot>)},
            {const_cast<char*>("channel"), const_cast<char*>("(Ljava/lang/String;Z)V"),
             reinterpret_cast<void*>(nativeChannel<Slot>)},
        }};
    }

private:
    template <typename Body>
    void call(JNIEnv* env, const char* name, const char* signature, Body&& body) const {
        jclass facade = env->FindClass(spec_.facade.c_str());
        if (facade == nullptr) {
            env->ExceptionClear();
            return;
        }
        jmethodID method = env->GetStaticMethodID(facade, name, signature);
        if (method != nullptr)
            body(facade, method);
        else
            env->ExceptionClear();
        if (env->ExceptionCheck()) {
            describe(env, name);
        }
        env->DeleteLocalRef(facade);
    }

    void describe(JNIEnv* env, const char* name) const {
        jthrowable error = env->ExceptionOccurred();
        env->ExceptionClear();
        std::string line = std::string{"a "} + spec_.label + " call failed in " + name;
        if (error != nullptr) {
            jclass type = env->FindClass("java/lang/Throwable");
            jmethodID toString = env->GetMethodID(type, "toString", "()Ljava/lang/String;");
            auto* described = static_cast<jstring>(env->CallObjectMethod(error, toString));
            if (described != nullptr)
                line += ": " + text(env, described);
            env->ExceptionClear();
            env->DeleteLocalRef(type);
            env->DeleteLocalRef(error);
        }
        if (hooks_.log)
            hooks_.log(spec_.logName, line);
    }

    [[nodiscard]] std::vector<std::string> callStrings(JNIEnv* env, const char* name) const {
        std::vector<std::string> values;
        call(env, name, "()[Ljava/lang/String;", [&](jclass facade, jmethodID method) {
            auto* array = static_cast<jobjectArray>(env->CallStaticObjectMethod(facade, method));
            if (!env->ExceptionCheck()) {
                values = strings(env, array);
                if (array != nullptr)
                    env->DeleteLocalRef(array);
            }
        });
        return values;
    }

    template <int Slot>
    static void nativeLog(JNIEnv* env, jclass, jstring line) {
        if (gAdapters[Slot] != nullptr && gAdapters[Slot]->hooks_.log)
            gAdapters[Slot]->hooks_.log(gAdapters[Slot]->spec_.logName, text(env, line));
    }

    template <int Slot>
    static jboolean nativeConnect(JNIEnv* env, jclass, jstring player, jstring server) {
        return gAdapters[Slot] != nullptr && gAdapters[Slot]->hooks_.send &&
                       gAdapters[Slot]->hooks_.send(text(env, player), text(env, server))
                   ? JNI_TRUE
                   : JNI_FALSE;
    }

    template <int Slot>
    static void nativeKick(JNIEnv* env, jclass, jstring player, jstring json) {
        if (gAdapters[Slot] == nullptr || !gAdapters[Slot]->hooks_.kickComponent)
            return;
        if (auto component = componentOf(text(env, json)))
            gAdapters[Slot]->hooks_.kickComponent(text(env, player), std::move(*component));
    }

    template <int Slot>
    static void nativeTell(JNIEnv* env, jclass, jstring player, jstring json) {
        if (gAdapters[Slot] == nullptr || !gAdapters[Slot]->hooks_.tellComponent)
            return;
        if (auto component = componentOf(text(env, json)))
            gAdapters[Slot]->hooks_.tellComponent(text(env, player), std::move(*component));
    }

    template <int Slot>
    static jboolean nativeSend(JNIEnv* env, jclass, jstring player, jstring channel, jbyteArray data, jboolean toward) {
        if (gAdapters[Slot] == nullptr || !gAdapters[Slot]->hooks_.sendMessage)
            return JNI_FALSE;
        const jsize size = env->GetArrayLength(data);
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
        env->GetByteArrayRegion(data, 0, size, reinterpret_cast<jbyte*>(bytes.data()));
        return gAdapters[Slot]->hooks_.sendMessage(text(env, player), text(env, channel), bytes, toward == JNI_TRUE)
                   ? JNI_TRUE
                   : JNI_FALSE;
    }

    template <int Slot>
    static jobjectArray nativeConfig(JNIEnv* env, jclass) {
        ProxyInfo info;
        if (gAdapters[Slot] != nullptr && gAdapters[Slot]->hooks_.info)
            info = gAdapters[Slot]->hooks_.info();
        const auto join = [](const std::vector<std::string>& values) {
            std::string joined;
            for (const std::string& value : values)
                joined += (joined.empty() ? "" : ",") + value;
            return joined;
        };
        std::string forced;
        for (const auto& [host, backends] : info.forcedHosts)
            forced += (forced.empty() ? "" : ";") + host + "=" + join(backends);
        return stringArray(env, {info.bind, std::to_string(info.port), info.onlineMode ? "1" : "0",
                                 std::to_string(info.compressionThreshold), std::to_string(info.connectTimeoutMs),
                                 std::to_string(info.readTimeoutMs), std::to_string(info.maxSessions),
                                 info.preventClientProxyConnections ? "1" : "0", join(info.defaultRoute), forced});
    }

    template <int Slot>
    static jobjectArray nativeServers(JNIEnv* env, jclass) {
        return stringArray(env, gAdapters[Slot] != nullptr && gAdapters[Slot]->hooks_.servers
                                    ? gAdapters[Slot]->hooks_.servers()
                                    : std::vector<std::string>{});
    }

    template <int Slot>
    static jstring nativeServerAddress(JNIEnv* env, jclass, jstring name) {
        if (gAdapters[Slot] == nullptr || !gAdapters[Slot]->hooks_.serverAddress)
            return nullptr;
        const auto address = gAdapters[Slot]->hooks_.serverAddress(text(env, name));
        if (!address)
            return nullptr;
        return env->NewStringUTF((address->first + ":" + std::to_string(address->second)).c_str());
    }

    template <int Slot>
    static jobjectArray nativePlayers(JNIEnv* env, jclass) {
        std::vector<PlayerRow> rows;
        if (gAdapters[Slot] != nullptr) {
            std::lock_guard lock{gAdapters[Slot]->mutex_};
            for (const auto& [name, row] : gAdapters[Slot]->players_)
                rows.push_back(row);
        }
        jclass type = env->FindClass("[Ljava/lang/String;");
        jobjectArray result = env->NewObjectArray(static_cast<jsize>(rows.size()), type, nullptr);
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const PlayerRow& row = rows[i];
            jobjectArray cells = stringArray(
                env, {row.name, row.uuid, row.server, row.address, std::to_string(row.port), std::to_string(row.ping)});
            env->SetObjectArrayElement(result, static_cast<jsize>(i), cells);
            env->DeleteLocalRef(cells);
        }
        env->DeleteLocalRef(type);
        return result;
    }

    template <int Slot>
    static void nativeChannel(JNIEnv* env, jclass, jstring name, jboolean add) {
        if (gAdapters[Slot] == nullptr)
            return;
        std::lock_guard lock{gAdapters[Slot]->mutex_};
        if (add == JNI_TRUE)
            gAdapters[Slot]->channels_.insert(text(env, name));
        else
            gAdapters[Slot]->channels_.erase(text(env, name));
    }

    std::shared_ptr<ShimHost> shim_;
    PluginHooks hooks_;
    JavaSpec spec_;
    mutable std::mutex mutex_;
    std::map<std::string, PlayerRow, std::less<>> players_;
    std::set<std::string, std::less<>> channels_;
};

std::unique_ptr<ForeignAdapter> createJavaAdapter(const JavaRuntimeOptions& options, const PluginHooks& hooks,
                                                  JavaSpec spec) {
    const auto log = [&hooks, &spec](std::string_view line) {
        if (hooks.log)
            hooks.log(spec.logName, line);
    };
    const auto jars = detectJars(options.pluginsDirectory);
    const auto has = [&jars](ForeignKind kind) {
        return std::ranges::any_of(jars, [kind](const DetectedJar& jar) { return jar.kind == kind; });
    };
    if (!has(spec.kind))
        return nullptr;
    namespace fs = std::filesystem;
    const fs::path root = fs::absolute(options.runtimeDirectory);
    static std::weak_ptr<ShimHost> weak;
    auto shared = weak.lock();
    if (!shared) {
#if defined(__APPLE__)
        const fs::path libjvm = root / "shim" / "libjvm.dylib";
#elif defined(_WIN32)
        const fs::path libjvm = root / "shim" / "jvm.dll";
#else
        const fs::path libjvm = root / "shim" / "libjvm.so";
#endif
        const fs::path boot = root / "shim" / "classpath.jar";
        for (const fs::path& needed : {libjvm, boot}) {
            if (!fs::exists(needed)) {
                log("Java plugins are present but " + needed.string() + " is missing.");
                return nullptr;
            }
        }
        wire::shim::ShimOptions shimOptions{.libjvm = libjvm.string(), .bootClasspath = boot.string(), .classpath = {}};
        const std::pair<ForeignKind, std::pair<const char*, const char*>> kinds[] = {
            {ForeignKind::kBungee, {"bungee", "basalt-bungee.jar"}},
            {ForeignKind::kVelocity, {"velocity", "basalt-velocity.jar"}}};
        for (const auto& [kind, names] : kinds) {
            if (!has(kind))
                continue;
            const fs::path facade = root / names.first / names.second;
            if (!fs::exists(facade)) {
                log(facade.string() + " is missing.");
                continue;
            }
            shimOptions.classpath.push_back(facade.string());
            std::vector<std::string> libraries;
            std::error_code error;
            for (const auto& entry : fs::directory_iterator{root / names.first / "lib", error})
                if (entry.path().extension() == ".jar")
                    libraries.push_back(entry.path().string());
            std::ranges::sort(libraries);
            shimOptions.classpath.insert(shimOptions.classpath.end(), libraries.begin(), libraries.end());
        }
        auto shim = ShimHost::create(shimOptions);
        if (!shim) {
            log("could not start the Java machine: " + shim.error());
            return nullptr;
        }
        shared = std::move(*shim);
        weak = shared;
    }
    auto adapter = std::make_unique<JavaAdapter>(shared, hooks, std::move(spec));
    if (const auto booted = adapter->boot(fs::absolute(options.pluginsDirectory).string()); !booted) {
        log(booted.error());
        return nullptr;
    }
    return adapter;
}

} // namespace basalt::proxy::plugins

#pragma once

#include <basalt/proxy/config/ProxyConfig.hpp>
#include <basalt/wire/common/toml/Toml.hpp>

#include <algorithm>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace basalt::proxy::config::detail {

using wire::common::toml::TomlTable;
using wire::common::toml::TomlValue;

struct Reader {
    std::string path;
    std::optional<std::string> error;

    void fail(std::string_view field, std::string_view reason) {
        if (!error)
            error = path + ": " + std::string{field} + " " + std::string{reason};
    }

    void keys(const TomlTable* table, std::initializer_list<std::string_view> allowed, std::string_view field) {
        if (!table)
            return;
        for (const auto& key : table->keys()) {
            if (std::find(allowed.begin(), allowed.end(), key) == allowed.end())
                fail(std::string{field} + key, "is unknown");
        }
    }

    const TomlTable* child(const TomlTable& table, std::string_view key) {
        const auto* value = table.find(key);
        if (!value)
            return nullptr;
        const auto* result = value->get<TomlTable>();
        if (!result)
            fail(key, "must be a table");
        return result;
    }

    template <typename T>
    void scalar(const TomlTable* table, std::string_view key, T& destination, std::string_view prefix) {
        if (!table)
            return;
        if (const auto* value = table->find(key)) {
            const auto* typed = value->get<T>();
            if (!typed)
                fail(std::string{prefix} + std::string{key}, "has the wrong type");
            else
                destination = *typed;
        }
    }

    template <typename T>
    void integer(const TomlTable* table, std::string_view key, T& destination, std::int64_t min, std::int64_t max,
                 std::string_view prefix) {
        if (!table)
            return;
        if (const auto* value = table->find(key)) {
            const auto* typed = value->get<std::int64_t>();
            if (!typed || *typed < min || *typed > max)
                fail(std::string{prefix} + std::string{key}, "is outside its integer range");
            else
                destination = static_cast<T>(*typed);
        }
    }

    std::vector<std::string> strings(const TomlValue* value, std::string_view field) {
        std::vector<std::string> result;
        const auto* list = value ? value->get<std::vector<TomlValue>>() : nullptr;
        if (!list || list->empty() || list->size() > 256) {
            fail(field, "must be a nonempty array of at most 256 backend names");
            return result;
        }
        for (const auto& element : *list) {
            const auto* text = element.get<std::string>();
            if (!text || text->empty()) {
                fail(field, "must contain only nonempty strings");
                return {};
            }
            if (std::find(result.begin(), result.end(), *text) != result.end()) {
                fail(field, "contains a duplicate backend");
                return {};
            }
            result.push_back(*text);
        }
        return result;
    }
};

[[nodiscard]] std::expected<void, std::string> readBackends(ProxyConfig& config, const TomlTable& root,
                                                            std::string_view path);

} // namespace basalt::proxy::config::detail

#pragma once

#include "PluginInternal.hpp"

#include <basalt/proxy/plugins/BungeeMessaging.hpp>
#include <basalt/wire/common/ByteView.hpp>
#include <basalt/wire/io/FileIO.hpp>
#include <basalt/wire/plugins/CommandArguments.hpp>
#include <basalt/wire/plugins/PacketView.hpp>
#include <basalt/wire/plugins/PluginFolder.hpp>
#include <basalt/wire/plugins/PluginOrder.hpp>
#include <basalt/wire/storage/Schema.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <filesystem>

namespace basalt::proxy::plugins {
namespace impl {

constexpr int kStrikeLimit = 5;

[[nodiscard]] inline std::vector<std::string> wordsOf(std::string_view line, std::size_t limit) {
    std::vector<std::string> words;
    while (!line.empty() && words.size() <= limit) {
        const auto begin = line.find_first_not_of(" \t\r\n");
        if (begin == std::string_view::npos)
            break;
        line.remove_prefix(begin);
        const auto end = line.find_first_of(" \t\r\n");
        words.emplace_back(line.substr(0, end));
        if (end == std::string_view::npos)
            break;
        line.remove_prefix(end);
    }
    return words;
}

[[nodiscard]] inline bool sameName(std::string_view left, std::string_view right) {
    return left.size() == right.size() && std::ranges::equal(left, right, [](unsigned char a, unsigned char b) {
               return std::tolower(a) == std::tolower(b);
           });
}

} // namespace impl

using namespace impl;

} // namespace basalt::proxy::plugins

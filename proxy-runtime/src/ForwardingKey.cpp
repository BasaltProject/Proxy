#include "ForwardingKey.hpp"

#include <basalt/wire/common/logging/Logger.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace basalt::proxy::runtime::detail {
namespace {

namespace native = wire::auth::forwarding::native;

int hexValue(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

bool writeOwnerOnly(const std::filesystem::path& path, const std::string& text) {
#if defined(_WIN32)
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    out << text;
    return static_cast<bool>(out);
#else
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0)
        return false;
    std::size_t written = 0;
    while (written < text.size()) {
        const auto count = ::write(fd, text.data() + written, text.size() - written);
        if (count <= 0) {
            ::close(fd);
            return false;
        }
        written += static_cast<std::size_t>(count);
    }
    return ::close(fd) == 0;
#endif
}

void announce(const native::SigningKey& key) {
    const wire::common::Logger logger{"Proxy"};
    logger.info([&key] {
        return "Forwarding key " + native::keyId(key.publicKey()) + ". Add to each basalt backend: trusted-keys = [\"" +
               hexEncode(key.publicKey()) + "\"]";
    });
}

} // namespace

std::string hexEncode(std::span<const std::uint8_t> bytes) {
    constexpr std::string_view kHex = "0123456789abcdef";
    std::string text;
    text.reserve(bytes.size() * 2);
    for (const std::uint8_t byte : bytes) {
        text.push_back(kHex[byte >> 4]);
        text.push_back(kHex[byte & 0xF]);
    }
    return text;
}

std::expected<native::SigningKey, std::string> loadOrCreateForwardingKey(std::string_view pathText) {
    const std::filesystem::path path{pathText};
    std::error_code ignored;
    if (std::filesystem::exists(path, ignored)) {
        std::ifstream in{path, std::ios::binary};
        std::ostringstream content;
        content << in.rdbuf();
        std::string text = content.str();
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
            text.pop_back();
        if (!in || text.size() != 64)
            return std::unexpected("forwarding key file " + path.string() + " must hold 64 hex characters");
        std::array<std::uint8_t, 32> seed{};
        for (std::size_t i = 0; i < seed.size(); ++i) {
            const int high = hexValue(text[2 * i]);
            const int low = hexValue(text[2 * i + 1]);
            if (high < 0 || low < 0)
                return std::unexpected("forwarding key file " + path.string() + " must hold 64 hex characters");
            seed[i] = static_cast<std::uint8_t>(high << 4 | low);
        }
        auto key = native::SigningKey::fromSeed(seed);
        announce(key);
        return key;
    }
    auto generated = native::SigningKey::generate();
    if (!generated)
        return std::unexpected("could not generate the forwarding key");
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path(), ignored);
    if (!writeOwnerOnly(path, hexEncode(generated->seed()) + "\n"))
        return std::unexpected("could not write the forwarding key file " + path.string());
    announce(*generated);
    return std::move(*generated);
}

} // namespace basalt::proxy::runtime::detail

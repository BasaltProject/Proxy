#include "Messages.hpp"

#include <basalt/wire/common/placeholder/PlaceholderText.hpp>
#include <basalt/wire/text/MiniMessage.hpp>
#include <basalt/wire/text/Serializers.hpp>

namespace basalt::proxy::runtime::detail {

std::string Messages::escape(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const char c : value) {
        if (c == '<' || c == '\\')
            result += '\\';
        result += c;
    }
    return result;
}

namespace {

void replaceAll(std::string& text, std::string_view token, std::string_view value) {
    for (std::size_t at = text.find(token); at != std::string::npos; at = text.find(token, at + value.size()))
        text.replace(at, token.size(), value);
}

} // namespace

Messages::Messages(config::MessageSet initial, wire::config::BasicFacts facts) : texts_(std::move(initial)) {
    wire::config::registerBasicPlaceholders(placeholders_, std::move(facts));
}

std::string Messages::fill(std::string_view text) const {
    return wire::common::placeholder::substitute(
        text, placeholders_, {},
        [](std::string_view, const wire::common::placeholder::PlaceholderValue&) -> std::optional<std::string> {
            return std::string{};
        });
}

std::string Messages::render(std::string_view section, std::string_view key, const MessageArgs& args) const {
    std::string text = texts_.current()->text(section, key);
    for (const auto& [name, value] : args)
        replaceAll(text, "%" + std::string{name} + "%", escape(value));
    return fill(text);
}

std::string Messages::plain(std::string_view miniMessage) {
    return wire::text::toPlainText(wire::text::miniMessage::parse(miniMessage));
}

} // namespace basalt::proxy::runtime::detail

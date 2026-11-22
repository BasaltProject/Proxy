#include "PluginInternal.hpp"

#include <basalt/wire/common/json/Json.hpp>
#include <basalt/wire/text/ComponentJson.hpp>

namespace basalt::proxy::plugins {

std::optional<wire::text::Component> componentOfJson(std::string_view json) {
    const auto parsed = wire::common::parseJson(json);
    if (!parsed)
        return std::nullopt;
    return wire::text::componentJson::deserialize(*parsed);
}

runtime::CommandNode commandNode(const wire::plugins::CommandSpec& spec, runtime::CommandExecutor executor) {
    using Type = wire::plugins::CommandArgumentType;
    std::vector<runtime::CommandNode> chain;
    chain.push_back(runtime::CommandNode::literal(spec.literals.front()));
    for (std::size_t i = 1; i < spec.literals.size(); ++i)
        chain.push_back(runtime::CommandNode::literal(spec.literals[i]));
    for (const auto& argument : spec.arguments) {
        const auto kind = argument.type == Type::Text    ? runtime::ArgumentKind::kText
                          : argument.type == Type::Int   ? runtime::ArgumentKind::kInt
                          : argument.type == Type::Float ? runtime::ArgumentKind::kFloat
                          : argument.type == Type::Bool  ? runtime::ArgumentKind::kBool
                                                         : runtime::ArgumentKind::kWord;
        chain.push_back(
            runtime::CommandNode::argument(argument.name, kind, argument.optional).range(argument.min, argument.max));
    }
    runtime::CommandNode node = std::move(chain.back());
    node.executes(std::move(executor));
    chain.pop_back();
    while (!chain.empty()) {
        runtime::CommandNode parent = std::move(chain.back());
        chain.pop_back();
        parent.then(std::move(node));
        node = std::move(parent);
    }
    return node;
}

} // namespace basalt::proxy::plugins

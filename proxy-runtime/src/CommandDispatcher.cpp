#include <basalt/proxy/runtime/CommandDispatcher.hpp>
#include <basalt/proxy/runtime/ProxyRuntime.hpp>
#include <basalt/wire/common/logging/Logger.hpp>
#include <basalt/wire/common/placeholder/Placeholders.hpp>

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <set>

namespace basalt::proxy::runtime {
namespace {

using wire::common::placeholder::lowercaseAscii;

constexpr std::size_t kMaxSuggestions = 100;
constexpr std::size_t kMaxWords = 64;

struct Token {
    std::string_view text;
    std::size_t begin = 0;
};

std::vector<Token> tokenize(std::string_view line) {
    std::vector<Token> tokens;
    std::size_t at = 0;
    while (at < line.size() && tokens.size() < kMaxWords) {
        const auto begin = line.find_first_not_of(' ', at);
        if (begin == std::string_view::npos)
            break;
        const auto end = line.find(' ', begin);
        tokens.push_back(
            {line.substr(begin, end == std::string_view::npos ? std::string_view::npos : end - begin), begin});
        if (end == std::string_view::npos)
            break;
        at = end;
    }
    return tokens;
}

std::string_view normalized(std::string_view line) {
    const auto first = line.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos)
        return {};
    line.remove_prefix(first);
    if (line.starts_with('/'))
        line.remove_prefix(1);
    while (!line.empty() && (line.back() == ' ' || line.back() == '\r' || line.back() == '\n' || line.back() == '\t'))
        line.remove_suffix(1);
    return line;
}

bool sameWord(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::ranges::equal(a, b, [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

} // namespace

std::string_view CommandContext::text(std::string_view name) const {
    const auto found = values_.find(name);
    return found == values_.end() ? std::string_view{} : std::string_view{found->second};
}

std::int64_t CommandContext::integer(std::string_view name) const {
    std::int64_t value = 0;
    const auto word = text(name);
    std::from_chars(word.data(), word.data() + word.size(), value);
    return value;
}

double CommandContext::number(std::string_view name) const {
    const std::string word{text(name)};
    return std::strtod(word.c_str(), nullptr);
}

void CommandContext::reply(std::string_view miniMessage) const {
    if (source_->reply)
        source_->reply(miniMessage);
}

CommandNode CommandNode::literal(std::string name) {
    CommandNode node;
    node.name_ = std::move(name);
    return node;
}

CommandNode CommandNode::argument(std::string name, ArgumentKind kind, bool optional) {
    CommandNode node;
    node.name_ = std::move(name);
    node.isLiteral_ = false;
    node.kind_ = kind;
    node.optional_ = optional;
    return node;
}

CommandNode& CommandNode::then(CommandNode child) {
    children_.push_back(std::move(child));
    return *this;
}

CommandNode& CommandNode::executes(CommandExecutor executor) {
    executor_ = std::move(executor);
    return *this;
}

CommandNode& CommandNode::range(std::optional<std::int64_t> min, std::optional<std::int64_t> max) {
    min_ = min;
    max_ = max;
    return *this;
}

CommandNode& CommandNode::requires_(std::string permission) {
    permission_ = std::move(permission);
    return *this;
}

namespace {

bool valid(const CommandNode& node, ArgumentKind kind, std::optional<std::int64_t> min, std::optional<std::int64_t> max,
           std::string_view word) {
    switch (kind) {
    case ArgumentKind::Int: {
        std::int64_t number = 0;
        const auto [end, error] = std::from_chars(word.data(), word.data() + word.size(), number);
        return error == std::errc{} && end == word.data() + word.size() && (!min || number >= *min) &&
               (!max || number <= *max);
    }
    case ArgumentKind::Float: {
        const std::string copy{word};
        char* end = nullptr;
        std::strtod(copy.c_str(), &end);
        return !copy.empty() && end == copy.c_str() + copy.size();
    }
    case ArgumentKind::Bool:
        return word == "true" || word == "false";
    default:
        static_cast<void>(node);
        return !word.empty();
    }
}

} // namespace

struct CommandDispatcher::Entry {
    CommandRegistration registration;
    std::vector<std::string> aliases;
};

namespace {
struct Resolved {
    std::shared_ptr<const void> keepAlive;
    const CommandRegistration* registration = nullptr;
    std::string permission;
    std::string name;
};
} // namespace

struct CommandDispatcher::Index {
    std::map<std::string, std::vector<Resolved>, std::less<>> byName;
    std::vector<std::shared_ptr<Entry>> custom;
    bool logCommands = true;
};

CommandDispatcher::CommandDispatcher(PermissionCheck permitted, CommandSuggestions suggestions)
    : permitted_(std::move(permitted)), suggestions_(std::move(suggestions)), index_(std::make_shared<Index>()) {}

std::expected<void, std::string> CommandDispatcher::add(CommandRegistration registration) {
    const std::string name = lowercaseAscii(registration.root.name());
    if (name.empty())
        return std::unexpected("a command needs a name");
    const bool plugin = registration.owner.starts_with("plugin:");
    {
        std::unique_lock lock{mutex_};
        for (const auto& entry : entries_) {
            const auto& other = entry->registration;
            const bool clashes =
                lowercaseAscii(other.root.name()) == name ||
                std::ranges::any_of(other.aliases, [&](auto& alias) { return lowercaseAscii(alias) == name; });
            if (clashes && !(plugin && other.owner.starts_with("plugin:")))
                return std::unexpected("'" + name + "' is already a command of " + other.owner);
        }
        auto entry = std::make_shared<Entry>();
        entry->registration = std::move(registration);
        entries_.push_back(std::move(entry));
    }
    rebuild();
    return {};
}

void CommandDispatcher::removeOwners(std::string_view ownerPrefix) {
    {
        std::unique_lock lock{mutex_};
        std::erase_if(entries_, [&](const auto& entry) { return entry->registration.owner.starts_with(ownerPrefix); });
    }
    rebuild();
}

void CommandDispatcher::configure(std::shared_ptr<const wire::config::CommandsConfig> config) {
    {
        std::unique_lock lock{mutex_};
        config_ = std::move(config);
    }
    rebuild();
}

void CommandDispatcher::renderCustomWith(TextRenderer renderer) {
    {
        std::unique_lock lock{mutex_};
        renderer_ = std::move(renderer);
    }
    rebuild();
}

void CommandDispatcher::rebuild() {
    const wire::common::Logger logger{"Commands"};
    std::unique_lock lock{mutex_};
    auto next = std::make_shared<Index>();
    const auto config = config_;
    const auto renderer = renderer_;
    std::vector<std::shared_ptr<Entry>> all = entries_;
    if (config) {
        next->logCommands = config->options().logCommands;
        for (const auto& custom : config->custom()) {
            auto entry = std::make_shared<Entry>();
            entry->registration.owner = "config";
            entry->registration.permission = custom.permission;
            entry->registration.aliases = custom.aliases;
            entry->registration.description = "A command of this network.";
            entry->registration.root = CommandNode::literal(custom.name);
            entry->registration.root.executes([lines = custom.message, renderer](const CommandContext& context) {
                for (const std::string& line : lines)
                    context.reply(renderer ? renderer(line) : line);
            });
            next->custom.push_back(entry);
            all.push_back(entry);
        }
    }
    static const wire::config::CommandOverride untouched;
    for (const auto& entry : all) {
        const auto& registration = entry->registration;
        const auto rootName = lowercaseAscii(registration.root.name());
        const wire::config::CommandOverride& override = config ? config->get(rootName) : untouched;
        if (!override.enabled)
            continue;
        std::vector<std::string> names{rootName};
        for (const auto& alias : registration.aliases)
            names.push_back(lowercaseAscii(alias));
        for (const auto& alias : override.aliases)
            names.push_back(lowercaseAscii(alias));
        for (const std::string& name : names) {
            auto& slot = next->byName[name];
            const bool plugin = registration.owner.starts_with("plugin:");
            if (!slot.empty() && !(plugin && slot.front().registration->owner.starts_with("plugin:"))) {
                logger.warn([&] {
                    return "The name '" + name + "' is taken by another command; '" + rootName +
                           "' does not answer to it.";
                });
                continue;
            }
            slot.push_back({entry, &registration, override.permission.value_or(registration.permission), rootName});
        }
    }
    std::erase_if(next->byName, [](const auto& item) { return item.second.empty(); });
    index_ = std::move(next);
}

std::shared_ptr<const CommandDispatcher::Index> CommandDispatcher::index() const {
    std::shared_lock lock{mutex_};
    return index_;
}

bool CommandDispatcher::available(const CommandSource& source, std::string_view line) const {
    const auto tokens = tokenize(normalized(line));
    if (tokens.empty())
        return false;
    const auto snapshot = index();
    const auto found = snapshot->byName.find(lowercaseAscii(tokens.front().text));
    return found != snapshot->byName.end() && std::ranges::any_of(found->second, [&](const Resolved& resolved) {
               return resolved.permission.empty() || permitted_(source, resolved.permission);
           });
}

bool CommandDispatcher::logCommands() const { return index()->logCommands; }

bool CommandDispatcher::knows(std::string_view line) const {
    const auto tokens = tokenize(normalized(line));
    if (tokens.empty())
        return false;
    return index()->byName.contains(lowercaseAscii(tokens.front().text));
}

namespace {

struct Walk {
    std::map<std::string, std::string, std::less<>> values;
    const CommandNode* deepest = nullptr;
    std::size_t deepestAt = 0;
    std::vector<std::string> path;
    std::vector<std::string> deepestPath;
    bool failed = false;
};

} // namespace

namespace {

std::string tokenOf(const CommandNode& node, bool optional, bool literal) {
    if (literal)
        return node.name();
    return optional ? "[" + node.name() + "]" : "<" + node.name() + ">";
}

} // namespace

CommandResult CommandDispatcher::execute(const CommandSource& source, std::string_view rawLine) const {
    const std::string_view line = normalized(rawLine);
    const auto tokens = tokenize(line);
    if (tokens.empty())
        return {};
    const auto snapshot = index();
    const auto roots = snapshot->byName.find(lowercaseAscii(tokens.front().text));
    if (roots == snapshot->byName.end())
        return {};
    bool denied = false;
    std::vector<std::string> usage;
    for (const Resolved& resolved : roots->second) {
        if (!resolved.permission.empty() && !permitted_(source, resolved.permission)) {
            denied = true;
            continue;
        }
        denied = false;
        const CommandNode& root = resolved.registration->root;
        Walk walk{{}, &root, 0, {}, {}};
        walk.path = {resolved.name};
        walk.deepestPath = walk.path;
        std::function<bool(const CommandNode&, std::size_t)> step;
        std::function<bool(const CommandNode&)> finish = [&](const CommandNode& node) -> bool {
            if (node.executor_) {
                const CommandContext context{source, line, walk.values};
                node.executor_(context);
                walk.failed = context.failed();
                return true;
            }
            return std::ranges::any_of(node.children_, [&](const CommandNode& child) {
                return !child.isLiteral_ && child.optional_ && finish(child);
            });
        };
        step = [&](const CommandNode& node, std::size_t at) -> bool {
            if (at >= walk.deepestAt) {
                walk.deepest = &node;
                walk.deepestAt = at;
                walk.deepestPath = walk.path;
            }
            if (at == tokens.size())
                return finish(node);
            const Token& token = tokens[at];
            for (const CommandNode& child : node.children_) {
                if (!child.isLiteral_ || !sameWord(child.name_, token.text))
                    continue;
                if (!child.permission_.empty() && !permitted_(source, child.permission_))
                    continue;
                walk.path.push_back(child.name_);
                if (step(child, at + 1))
                    return true;
                walk.path.pop_back();
            }
            for (const CommandNode& child : node.children_) {
                if (child.isLiteral_)
                    continue;
                std::string value{child.kind_ == ArgumentKind::Text ? line.substr(token.begin) : token.text};
                if (!valid(child, child.kind_, child.min_, child.max_, value))
                    continue;
                walk.values[child.name_] = std::move(value);
                walk.path.push_back(tokenOf(child, false, false));
                if (step(child, child.kind_ == ArgumentKind::Text ? tokens.size() : at + 1))
                    return true;
                walk.path.pop_back();
                walk.values.erase(child.name_);
            }
            return false;
        };
        if (step(root, 1))
            return {walk.failed ? CommandOutcome::Failed : CommandOutcome::Executed, {}};
        std::string prefix;
        for (const std::string& word : walk.deepestPath)
            prefix += (prefix.empty() ? "" : " ") + word;
        std::function<void(const CommandNode&, const std::string&)> collect = [&](const CommandNode& node,
                                                                                  const std::string& at) {
            if (node.executor_)
                usage.push_back(at);
            for (const CommandNode& child : node.children_)
                collect(child, at + " " + tokenOf(child, child.optional_, child.isLiteral_));
        };
        collect(*walk.deepest, prefix);
        if (usage.empty())
            usage.push_back(prefix);
    }
    if (!usage.empty())
        return {CommandOutcome::Usage, std::move(usage)};
    return {denied ? CommandOutcome::Denied : CommandOutcome::Usage, {}};
}

std::vector<std::string> CommandDispatcher::roots(const CommandSource& source) const {
    std::vector<std::string> names;
    for (const auto& [name, list] : index()->byName)
        if (std::ranges::any_of(list, [&](const Resolved& resolved) {
                return resolved.permission.empty() || permitted_(source, resolved.permission);
            }))
            names.push_back(name);
    return names;
}

std::vector<std::string> CommandDispatcher::suggest(const CommandSource& source, std::string_view rawLine) const {
    std::string_view line = rawLine;
    if (line.starts_with('/'))
        line.remove_prefix(1);
    auto tokens = tokenize(line);
    const bool fresh = line.empty() || line.back() == ' ';
    std::string partial;
    if (!fresh && !tokens.empty()) {
        partial = std::string{tokens.back().text};
        tokens.pop_back();
    }
    const auto lower = lowercaseAscii(partial);
    std::set<std::string> found;
    const auto offer = [&](std::string candidate) {
        if (lowercaseAscii(candidate).starts_with(lower))
            found.insert(std::move(candidate));
    };
    if (tokens.empty()) {
        for (std::string& name : roots(source))
            offer(std::move(name));
    } else {
        const auto snapshot = index();
        const auto list = snapshot->byName.find(lowercaseAscii(tokens.front().text));
        if (list != snapshot->byName.end()) {
            for (const Resolved& resolved : list->second) {
                if (!resolved.permission.empty() && !permitted_(source, resolved.permission))
                    continue;
                const CommandNode* node = &resolved.registration->root;
                bool lost = false;
                for (std::size_t at = 1; at < tokens.size() && !lost; ++at) {
                    const CommandNode* next = nullptr;
                    for (const CommandNode& child : node->children_)
                        if (child.isLiteral_ && sameWord(child.name_, tokens[at].text))
                            next = &child;
                    for (const CommandNode& child : node->children_)
                        if (next == nullptr && !child.isLiteral_ &&
                            valid(child, child.kind_, child.min_, child.max_, tokens[at].text))
                            next = &child;
                    lost = next == nullptr || next->kind_ == ArgumentKind::Text;
                    if (!lost)
                        node = next;
                }
                if (lost)
                    continue;
                for (const CommandNode& child : node->children_) {
                    if (!child.permission_.empty() && !permitted_(source, child.permission_))
                        continue;
                    if (child.isLiteral_) {
                        offer(child.name_);
                        continue;
                    }
                    std::vector<std::string> names;
                    if (child.kind_ == ArgumentKind::Player && suggestions_.players)
                        names = suggestions_.players();
                    else if (child.kind_ == ArgumentKind::Server && suggestions_.servers)
                        names = suggestions_.servers();
                    else if (child.kind_ == ArgumentKind::Group && suggestions_.groups)
                        names = suggestions_.groups();
                    else if (child.kind_ == ArgumentKind::Bool)
                        names = {"true", "false"};
                    for (std::string& name : names)
                        offer(std::move(name));
                }
            }
        }
    }
    std::vector<std::string> result{found.begin(), found.end()};
    if (result.size() > kMaxSuggestions)
        result.resize(kMaxSuggestions);
    return result;
}

std::vector<PlayerCommandSpec> CommandDispatcher::specs(const CommandSource& source) const {
    using Kind = PlayerCommandSpec::Kind;
    std::vector<PlayerCommandSpec> result;
    for (const auto& [name, list] : index()->byName) {
        for (const Resolved& resolved : list) {
            if (!resolved.permission.empty() && !permitted_(source, resolved.permission))
                continue;
            std::function<void(const CommandNode&, PlayerCommandSpec)> walk = [&](const CommandNode& node,
                                                                                  PlayerCommandSpec spec) {
                if (node.executor_ || (node.children_.empty() && &node != &resolved.registration->root))
                    result.push_back(spec);
                for (const CommandNode& child : node.children_) {
                    if (!child.permission_.empty() && !permitted_(source, child.permission_))
                        continue;
                    PlayerCommandSpec next = spec;
                    if (child.isLiteral_) {
                        if (!next.arguments.empty())
                            continue;
                        next.literals.push_back(child.name_);
                    } else {
                        const Kind kind = child.kind_ == ArgumentKind::Text    ? Kind::kText
                                          : child.kind_ == ArgumentKind::Int   ? Kind::kInt
                                          : child.kind_ == ArgumentKind::Float ? Kind::kFloat
                                          : child.kind_ == ArgumentKind::Bool  ? Kind::kBool
                                                                               : Kind::kWord;
                        const bool asks = child.kind_ == ArgumentKind::Player || child.kind_ == ArgumentKind::Server ||
                                          child.kind_ == ArgumentKind::Group;
                        next.arguments.push_back({child.name_, kind, child.optional_, child.min_, child.max_, asks});
                    }
                    walk(child, std::move(next));
                }
            };
            PlayerCommandSpec start;
            start.literals = {name};
            walk(resolved.registration->root, std::move(start));
        }
    }
    return result;
}

std::vector<HelpEntry> CommandDispatcher::help(const CommandSource& source) const {
    std::vector<HelpEntry> entries;
    std::set<const CommandRegistration*> seen;
    for (const auto& [name, list] : index()->byName) {
        for (const Resolved& resolved : list) {
            if (resolved.name != name || !seen.insert(resolved.registration).second)
                continue;
            if (!resolved.permission.empty() && !permitted_(source, resolved.permission))
                continue;
            std::string usage = name;
            const CommandNode* node = &resolved.registration->root;
            while (!node->children_.empty()) {
                if (node->children_.size() == 1) {
                    node = &node->children_.front();
                    usage += " " + tokenOf(*node, node->optional_, node->isLiteral_);
                    continue;
                }
                std::string alternatives;
                for (const CommandNode& child : node->children_)
                    alternatives += (alternatives.empty() ? "" : "|") + child.name_;
                usage += " <" + alternatives + ">";
                break;
            }
            entries.push_back({std::move(usage), resolved.registration->description});
        }
    }
    return entries;
}

} // namespace basalt::proxy::runtime

#pragma once

#include <basalt/wire/config/CommandsConfig.hpp>

#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

namespace basalt::proxy::runtime {

struct PlayerCommandSpec;

/// Player, Server and Group are single words that the dispatcher knows how to complete.
enum class ArgumentKind : std::uint8_t { Word, Text, Int, Float, Bool, Player, Server, Group };

struct CommandSource {
    std::string name;
    bool console = false;
    /// MiniMessage, sent to wherever the command came from.
    std::function<void(std::string_view)> reply;
};

class CommandContext {
public:
    CommandContext(const CommandSource& source, std::string_view line,
                   std::map<std::string, std::string, std::less<>> values)
        : source_(&source), line_(line), values_(std::move(values)) {}

    [[nodiscard]] const CommandSource& source() const noexcept { return *source_; }

    /// The whole line as typed, without the leading slash.
    [[nodiscard]] std::string_view line() const noexcept { return line_; }

    [[nodiscard]] bool has(std::string_view name) const { return values_.contains(name); }

    [[nodiscard]] std::string_view text(std::string_view name) const;

    [[nodiscard]] std::int64_t integer(std::string_view name) const;

    [[nodiscard]] double number(std::string_view name) const;

    [[nodiscard]] bool flag(std::string_view name) const { return text(name) == "true"; }

    void reply(std::string_view miniMessage) const;

private:
    const CommandSource* source_;
    std::string line_;
    std::map<std::string, std::string, std::less<>> values_;
};

using CommandExecutor = std::function<void(const CommandContext&)>;

class CommandNode {
public:
    [[nodiscard]] static CommandNode literal(std::string name);
    [[nodiscard]] static CommandNode argument(std::string name, ArgumentKind kind, bool optional = false);

    CommandNode& then(CommandNode child);
    CommandNode& executes(CommandExecutor executor);
    CommandNode& range(std::optional<std::int64_t> min, std::optional<std::int64_t> max);
    CommandNode& requires_(std::string permission);

    [[nodiscard]] const std::string& name() const noexcept { return name_; }

private:
    friend class CommandDispatcher;

    std::string name_;
    bool isLiteral_ = true;
    ArgumentKind kind_ = ArgumentKind::Word;
    bool optional_ = false;
    std::optional<std::int64_t> min_;
    std::optional<std::int64_t> max_;
    std::string permission_;
    CommandExecutor executor_;
    std::vector<CommandNode> children_;
};

struct CommandRegistration {
    /// A literal: the name the command answers to.
    CommandNode root = CommandNode::literal("");
    std::vector<std::string> aliases;
    /// Empty means everyone; commands.toml may replace it.
    std::string permission;
    std::string description;
    /// "basalt" for the proxy's own commands, "plugin:<id>" for a plugin's.
    std::string owner;
};

enum class CommandOutcome : std::uint8_t { Unknown, Denied, Usage, Executed };

struct CommandResult {
    CommandOutcome outcome = CommandOutcome::Unknown;
    /// Usage lines, without the slash, when the outcome is Usage.
    std::vector<std::string> usage;
};

struct HelpEntry {
    std::string usage;
    std::string description;
};

struct CommandSuggestions {
    std::function<std::vector<std::string>()> players;
    std::function<std::vector<std::string>()> servers;
    std::function<std::vector<std::string>()> groups;
};

/// One tree of commands for the console and for players, with permissions, aliases and tab completion.
class CommandDispatcher {
public:
    using PermissionCheck = std::function<bool(const CommandSource&, std::string_view node)>;
    using TextRenderer = std::function<std::string(std::string_view)>;

    CommandDispatcher(PermissionCheck permitted, CommandSuggestions suggestions);

    /// Fails when the name belongs to another owner's command.
    [[nodiscard]] std::expected<void, std::string> add(CommandRegistration registration);

    void removeOwners(std::string_view ownerPrefix);

    /// Applies renames, removals, permissions and custom commands; the new configuration takes effect for the next line.
    void configure(std::shared_ptr<const wire::config::CommandsConfig> config);

    void renderCustomWith(TextRenderer renderer);

    /// True when the line starts with a command that exists, so the caller can keep it from a backend.
    [[nodiscard]] bool knows(std::string_view line) const;

    /// Like `knows`, but only for a command the source may run; the rest belongs to the backend.
    [[nodiscard]] bool available(const CommandSource& source, std::string_view line) const;

    [[nodiscard]] CommandResult execute(const CommandSource& source, std::string_view line) const;

    [[nodiscard]] std::vector<std::string> suggest(const CommandSource& source, std::string_view line) const;

    /// Every executable path of every command the source may use, for the command tree sent to a client.
    [[nodiscard]] std::vector<PlayerCommandSpec> specs(const CommandSource& source) const;

    [[nodiscard]] std::vector<HelpEntry> help(const CommandSource& source) const;

    [[nodiscard]] bool logCommands() const;

    [[nodiscard]] std::vector<std::string> roots(const CommandSource& source) const;

private:
    struct Entry;
    struct Index;

    void rebuild();
    [[nodiscard]] std::shared_ptr<const Index> index() const;

    PermissionCheck permitted_;
    CommandSuggestions suggestions_;
    mutable std::shared_mutex mutex_;
    std::vector<std::shared_ptr<Entry>> entries_;
    std::shared_ptr<const wire::config::CommandsConfig> config_;
    TextRenderer renderer_;
    std::shared_ptr<const Index> index_;
};

} // namespace basalt::proxy::runtime

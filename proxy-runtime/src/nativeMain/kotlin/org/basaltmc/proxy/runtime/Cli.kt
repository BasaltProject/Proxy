package org.basaltmc.proxy.runtime

/** What the command line asked for. Parsing never exits or prints: [main] decides both. */
public sealed interface CliCommand {
    public data object ShowHelp : CliCommand

    public data object ShowVersion : CliCommand

    /** Start the proxy, reading its configuration from [configDirectory]. */
    public data class Run(
        val configDirectory: String,
    ) : CliCommand

    /** The arguments did not parse. [message] is one line, already addressed to an operator. */
    public data class Invalid(
        val message: String,
    ) : CliCommand
}

public object Cli {
    public const val DEFAULT_CONFIG_DIRECTORY: String = "./config"

    public fun parse(args: Array<String>): CliCommand {
        var configDirectory = DEFAULT_CONFIG_DIRECTORY
        var index = 0
        while (index < args.size) {
            when (val arg = args[index]) {
                "-h", "--help" -> return CliCommand.ShowHelp
                "-v", "--version" -> return CliCommand.ShowVersion
                "--config" -> {
                    val value =
                        args.getOrNull(index + 1)
                            ?: return CliCommand.Invalid("--config needs a directory, for example --config ./config")
                    // A following flag means the value was forgotten, not that a directory is named "--port".
                    if (value.startsWith("-")) {
                        return CliCommand.Invalid("--config needs a directory, but was followed by '$value'")
                    }
                    configDirectory = value
                    index++
                }
                else -> return CliCommand.Invalid("Unknown option '$arg'. Run with --help to see what is accepted.")
            }
            index++
        }
        return CliCommand.Run(configDirectory)
    }

    public fun helpText(): String =
        """
        Basalt Proxy ${BuildInfo.VERSION}

        Usage: proxy-runtime [options]

        Options:
          --config <dir>   Directory to read configuration from (default: $DEFAULT_CONFIG_DIRECTORY)
          -v, --version    Print the version and exit
          -h, --help       Print this help and exit
        """.trimIndent()

    public fun versionLine(): String =
        "Basalt Proxy ${BuildInfo.VERSION} (Minecraft ${BuildInfo.MINECRAFT_VERSION}, protocol ${BuildInfo.PROTOCOL_VERSION})"
}

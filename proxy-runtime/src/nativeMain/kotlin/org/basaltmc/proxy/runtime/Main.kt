package org.basaltmc.proxy.runtime

import org.basaltmc.wire.common.logging.LogOutput
import org.basaltmc.wire.common.logging.Logger
import platform.posix.STDOUT_FILENO
import platform.posix.isatty
import kotlin.system.exitProcess

/** Stable, so orchestrators and scripts can tell a bad command line from a crash. */
private const val USAGE_EXIT_CODE = 2

public fun main(args: Array<String>) {
    when (val command = Cli.parse(args)) {
        is CliCommand.ShowHelp -> println(Cli.helpText())
        is CliCommand.ShowVersion -> println(Cli.versionLine())
        is CliCommand.Invalid -> {
            println(command.message)
            exitProcess(USAGE_EXIT_CODE)
        }
        is CliCommand.Run -> run(command)
    }
}

private fun run(command: CliCommand.Run) {
    // Escape codes are a terminal affordance; piped or redirected output gets none.
    LogOutput.useColor = isatty(STDOUT_FILENO) == 1
    println(BANNER)
    println(Cli.versionLine())

    val logger = Logger("Proxy")
    logger.info { "Reading configuration from ${command.configDirectory}." }
    logger.warn { "Nothing is implemented yet: this build starts, prints and exits." }
    exitProcess(0)
}

// Raw string, and deliberately not indented: the art is full of backslashes, and every leading
// space in it is significant.
private const val BANNER =
    """
  ____                  _ _     _____
 |  _ \                | | |   |  __ \
 | |_) | __ _ ___  __ _| | |_  | |__) | __ _____  ___   _
 |  _ < / _` / __|/ _` | | __| |  ___/ '__/ _ \ \/ / | | |
 | |_) | (_| \__ \ (_| | | |_  | |   | | | (_) >  <| |_| |
 |____/ \__,_|___/\__,_|_|\__| |_|   |_|  \___/_/\_\\__, |
                                                     __/ |
                                                    |___/
"""

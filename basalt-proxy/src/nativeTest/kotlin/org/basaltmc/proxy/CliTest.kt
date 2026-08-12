package org.basaltmc.proxy

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertIs
import kotlin.test.assertTrue

class CliTest {
    @Test
    fun `no arguments runs with the default configuration directory`() {
        assertEquals(CliCommand.Run(Cli.DEFAULT_CONFIG_DIRECTORY), Cli.parse(emptyArray()))
    }

    @Test
    fun `help and version win over anything after them`() {
        assertIs<CliCommand.ShowHelp>(Cli.parse(arrayOf("--help", "--config", "x")))
        assertIs<CliCommand.ShowHelp>(Cli.parse(arrayOf("-h")))
        assertIs<CliCommand.ShowVersion>(Cli.parse(arrayOf("--version")))
        assertIs<CliCommand.ShowVersion>(Cli.parse(arrayOf("-v")))
    }

    @Test
    fun `config takes the following argument`() {
        assertEquals(CliCommand.Run("/etc/basalt"), Cli.parse(arrayOf("--config", "/etc/basalt")))
    }

    @Test
    fun `a config without a value is rejected rather than defaulted`() {
        assertIs<CliCommand.Invalid>(Cli.parse(arrayOf("--config")))
        assertIs<CliCommand.Invalid>(Cli.parse(arrayOf("--config", "--version")))
    }

    @Test
    fun `an unknown option names itself`() {
        val result = Cli.parse(arrayOf("--nope"))
        assertIs<CliCommand.Invalid>(result)
        assertTrue("--nope" in result.message)
    }

    @Test
    fun `help lists every option it accepts`() {
        val help = Cli.helpText()
        for (option in listOf("--config", "--version", "--help")) {
            assertTrue(option in help, "help does not mention $option")
        }
    }
}

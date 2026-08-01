package info.cemu.cemu.utils

import org.junit.Assert.assertEquals
import org.junit.Test

class OpenLastGameCommandTest {
    @Test
    fun `launches recorded game`() {
        var launchedPath: String? = null
        val command = OpenLastGameCommand(
            getLastGamePath = { "content://games/botw.wua" },
            isTitleRunning = { false },
            launchGame = { launchedPath = it },
        )

        assertEquals("open_last_game launched\n", command.execute(emptyList()))
        assertEquals("content://games/botw.wua", launchedPath)
    }

    @Test
    fun `does not replace a running title`() {
        val command = OpenLastGameCommand(
            getLastGamePath = { "content://games/botw.wua" },
            isTitleRunning = { true },
            launchGame = { error("must not launch") },
        )

        assertEquals(
            "open_last_game unavailable: a title is already running\n",
            command.execute(emptyList()),
        )
    }

    @Test
    fun `reports missing launch history`() {
        val command = OpenLastGameCommand(
            getLastGamePath = { null },
            isTitleRunning = { false },
            launchGame = { error("must not launch") },
        )

        assertEquals(
            "open_last_game unavailable: launch a game once to create history\n",
            command.execute(emptyList()),
        )
    }

    @Test
    fun `rejects arguments`() {
        val command = OpenLastGameCommand(
            getLastGamePath = { null },
            isTitleRunning = { false },
            launchGame = { error("must not launch") },
        )

        assertEquals("usage: open_last_game\n", command.execute(listOf("extra")))
    }
}

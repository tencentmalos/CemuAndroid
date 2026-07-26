package info.cemu.cemu.utils

import org.junit.Assert.assertEquals
import org.junit.Test

class DebugDumpRequestExecutorTest {
    @Test
    fun `executes directly on target thread`() {
        val executor = DebugDumpRequestExecutor(
            postToTarget = { false },
            isTargetThread = { true },
        )

        assertEquals("status\n", executor.execute { "status\n" })
    }

    @Test
    fun `posts request to target thread`() {
        val executor = DebugDumpRequestExecutor(
            postToTarget = {
                it.run()
                true
            },
            isTargetThread = { false },
        )

        assertEquals("pause succeeded\n", executor.execute { "pause succeeded\n" })
    }

    @Test
    fun `reports rejected request`() {
        val executor = DebugDumpRequestExecutor(
            postToTarget = { false },
            isTargetThread = { false },
        )

        assertEquals(
            "debugbus: target thread rejected request\n",
            executor.execute { "unreachable\n" },
        )
    }

    @Test
    fun `reports timeout without running request on binder thread`() {
        val executor = DebugDumpRequestExecutor(
            postToTarget = { true },
            isTargetThread = { false },
            timeoutMilliseconds = 1,
        )

        assertEquals(
            "debugbus: request timed out after 1ms\n",
            executor.execute { "unreachable\n" },
        )
    }

    @Test
    fun `reports command failure`() {
        val executor = DebugDumpRequestExecutor(
            postToTarget = {
                it.run()
                true
            },
            isTargetThread = { false },
        )

        assertEquals(
            "debugbus: request failed: broken\n",
            executor.execute { error("broken") },
        )
    }
}

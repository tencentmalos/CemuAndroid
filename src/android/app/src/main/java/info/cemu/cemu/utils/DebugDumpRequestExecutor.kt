package info.cemu.cemu.utils

import java.util.concurrent.ExecutionException
import java.util.concurrent.FutureTask
import java.util.concurrent.TimeUnit
import java.util.concurrent.TimeoutException

internal class DebugDumpRequestExecutor(
    private val postToTarget: (Runnable) -> Boolean,
    private val isTargetThread: () -> Boolean,
    private val timeoutMilliseconds: Long = DEFAULT_TIMEOUT_MILLISECONDS,
) {
    fun execute(request: () -> String): String {
        if (isTargetThread()) {
            return executeImmediately(request)
        }

        val task = FutureTask(request)
        if (!postToTarget(task)) {
            return "debugbus: target thread rejected request\n"
        }

        return try {
            task.get(timeoutMilliseconds, TimeUnit.MILLISECONDS)
        } catch (_: TimeoutException) {
            task.cancel(false)
            "debugbus: request timed out after ${timeoutMilliseconds}ms\n"
        } catch (_: InterruptedException) {
            Thread.currentThread().interrupt()
            "debugbus: request interrupted\n"
        } catch (exception: ExecutionException) {
            failureMessage(exception.cause ?: exception)
        }
    }

    private fun executeImmediately(request: () -> String): String = try {
        request()
    } catch (exception: Exception) {
        failureMessage(exception)
    }

    private fun failureMessage(exception: Throwable): String {
        val detail = exception.message ?: exception.javaClass.simpleName
        return "debugbus: request failed: $detail\n"
    }

    private companion object {
        const val DEFAULT_TIMEOUT_MILLISECONDS = 2_000L
    }
}

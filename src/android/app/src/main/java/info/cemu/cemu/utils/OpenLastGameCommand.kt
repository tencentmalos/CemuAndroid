package info.cemu.cemu.utils

internal class OpenLastGameCommand(
    private val getLastGamePath: () -> String?,
    private val isTitleRunning: () -> Boolean,
    private val launchGame: (String) -> Unit,
) {
    fun execute(args: List<String>): String {
        if (args.isNotEmpty()) {
            return "usage: open_last_game\n"
        }
        if (isTitleRunning()) {
            return "open_last_game unavailable: a title is already running\n"
        }

        val path = getLastGamePath()
            ?: return "open_last_game unavailable: launch a game once to create history\n"

        return try {
            launchGame(path)
            "open_last_game launched\n"
        } catch (exception: Exception) {
            val detail = exception.message ?: exception.javaClass.simpleName
            "open_last_game failed: $detail\n"
        }
    }
}

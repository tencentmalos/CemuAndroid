package info.cemu.cemu.common.gamelaunch

import android.content.Context

object LastGameStore {
    private const val PREFERENCES_NAME = "game_launch_history"
    private const val LAST_GAME_PATH = "last_game_path"

    fun record(context: Context, path: String) {
        if (path.isBlank()) {
            return
        }
        context.applicationContext
            .getSharedPreferences(PREFERENCES_NAME, Context.MODE_PRIVATE)
            .edit()
            .putString(LAST_GAME_PATH, path)
            .apply()
    }

    fun get(context: Context): String? = context.applicationContext
        .getSharedPreferences(PREFERENCES_NAME, Context.MODE_PRIVATE)
        .getString(LAST_GAME_PATH, null)
        ?.takeIf { it.isNotBlank() }
}

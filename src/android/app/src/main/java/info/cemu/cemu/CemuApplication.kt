package info.cemu.cemu

import android.app.Activity
import android.app.ActivityManager
import android.app.Application
import android.os.Build
import android.os.Bundle
import android.util.Log
import info.cemu.cemu.common.android.context.internalFolder
import info.cemu.cemu.common.settings.AppSettingsStore
import info.cemu.cemu.common.ui.localization.setLanguage
import info.cemu.cemu.common.ui.localization.setTranslations
import info.cemu.cemu.nativeinterface.NativeActiveSettings.initializeActiveSettings
import info.cemu.cemu.nativeinterface.NativeDebugDump.initialize as initializeDebugDump
import info.cemu.cemu.nativeinterface.NativeActiveSettings.setInternalDir
import info.cemu.cemu.nativeinterface.NativeActiveSettings.setNativeLibDir
import info.cemu.cemu.nativeinterface.NativeEmulation.initializeEmulation
import info.cemu.cemu.nativeinterface.NativeEmulation.setDPI
import info.cemu.cemu.nativeinterface.NativeFiles
import info.cemu.cemu.nativeinterface.NativeGraphicPacks.refreshGraphicPacks
import info.cemu.cemu.nativeinterface.NativeLogging.crashLog
import info.cemu.cemu.nativeinterface.NativeSwkbd.initializeSwkbd
import info.cemu.cemu.utils.DebugDumpService
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.runBlocking
import java.io.File
import java.io.IOException
import java.io.PrintWriter
import java.io.StringWriter
import java.util.regex.Pattern

class CemuApplication : Application() {
    override fun onCreate() {
        super.onCreate()

        Log.i(INIT_TAG, "Application.onCreate begin pid=${android.os.Process.myPid()}")
        logHistoricalProcessExits()

        configureExceptionHandler()

        Log.i(INIT_TAG, "AppSettingsStore.init begin")
        AppSettingsStore.init(this)
        Log.i(INIT_TAG, "AppSettingsStore.init end")

        NativeFiles.initialize(contentResolver)

        Log.i(INIT_TAG, "translations initialization begin")
        initializeTranslations()
        Log.i(INIT_TAG, "translations initialization end")

        Log.i(INIT_TAG, "native Cemu initialization begin")
        initializeCemu()
        Log.i(INIT_TAG, "native Cemu initialization end")
        initializeDebugDump()
        DebugDumpService.start(this)
        registerActivityLifecycleCallbacks(DebugDumpLifecycleCallbacks(this))

        saveDataFiles()
        Log.i(INIT_TAG, "Application.onCreate end")
    }

    override fun onTrimMemory(level: Int) {
        Log.w(INIT_TAG, "onTrimMemory level=$level")
        super.onTrimMemory(level)
    }

    override fun onLowMemory() {
        Log.e(INIT_TAG, "onLowMemory")
        super.onLowMemory()
    }

    private fun initializeTranslations() {
        setTranslations(this)

        val language = runBlocking {
            AppSettingsStore.dataStore.data.map { it.guiSettings.language }.first()
        }

        setLanguage(language, this)
    }

    private fun saveDataFiles() {
        val dataFolder = File(internalCemuDataFolder)

        if (!dataFolder.exists() && !dataFolder.mkdirs()) {
            return
        }

        val hashFileName = "hash.txt"
        val hashFile = dataFolder.resolve(hashFileName)
        val oldHash = if (hashFile.isFile) hashFile.readText() else "invalid"

        val newHash = try {
            assets.open(hashFileName).use { it.reader().readText() }
        } catch (_: IOException) {
            return
        }

        if (oldHash == newHash) {
            return
        }

        dataFolder.deleteRecursively()
        dataFolder.mkdirs()
        dataFolder.resolve(hashFileName).writeText(newHash)

        fun traverseAssets(path: String = ""): Iterator<String> = iterator {
            val assetFiles = assets.list(path) ?: return@iterator

            if (assetFiles.isEmpty()) {
                yield(path)
            }

            for (assetFile in assetFiles) {
                val assetPath = path + (if (path == "") "" else "/") + assetFile
                for (file in traverseAssets(assetPath)) {
                    yield(file)
                }
            }
        }

        val filePatterns = arrayOf(
            Pattern.compile("gameProfiles/.*"),
            Pattern.compile("resources/.*"),
        )

        fun isFileValid(file: String): Boolean {
            return filePatterns.any { pattern -> pattern.matcher(file).matches() }
        }

        for (assetFile in traverseAssets()) {
            if (!isFileValid(assetFile)) {
                continue
            }

            val outFile = dataFolder.resolve(assetFile)
            outFile.parentFile?.mkdirs()
            assets.open(assetFile)
                .use { asset -> outFile.outputStream().use { out -> asset.copyTo(out) } }
        }
    }

    private fun configureExceptionHandler() {
        if (DefaultUncaughtExceptionHandler == null) {
            DefaultUncaughtExceptionHandler = Thread.getDefaultUncaughtExceptionHandler()
        }
        Thread.setDefaultUncaughtExceptionHandler { thread: Thread, exception: Throwable ->
            Log.e(INIT_TAG, "uncaught exception thread=${thread.name}", exception)
            val stringWriter = StringWriter()
            val printWriter = PrintWriter(stringWriter)
            exception.printStackTrace(printWriter)
            val stacktrace = stringWriter.toString()
            crashLog(stacktrace)
            DefaultUncaughtExceptionHandler!!.uncaughtException(
                thread,
                exception,
            )
        }
    }

    private fun initializeCemu() {
        val displayMetrics = resources.displayMetrics
        setDPI(displayMetrics.density)
        initializeActiveSettings(
            userDataPath = internalCemuUserFolder,
            configPath = internalCemuUserFolder,
            dataPath = internalCemuDataFolder,
            cachePath = internalCemuUserFolder,
        )
        setNativeLibDir(applicationInfo.nativeLibraryDir)
        setInternalDir(dataDir.absolutePath)
        Log.i(INIT_TAG, "NativeEmulation.initializeEmulation begin")
        initializeEmulation()
        Log.i(INIT_TAG, "NativeEmulation.initializeEmulation end")
        initializeSwkbd()
        refreshGraphicPacks()
    }

    private fun logHistoricalProcessExits() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) {
            return
        }

        try {
            val activityManager = getSystemService(ActivityManager::class.java)
            activityManager.getHistoricalProcessExitReasons(packageName, 0, 3)
                .forEachIndexed { index, exit ->
                    Log.i(
                        INIT_TAG,
                        "previousExit[$index] timestamp=${exit.timestamp} reason=${exit.reason} " +
                            "status=${exit.status} importance=${exit.importance} " +
                            "pssKb=${exit.pss} rssKb=${exit.rss} description=${exit.description}",
                    )
                }
        } catch (exception: RuntimeException) {
            Log.w(INIT_TAG, "Unable to query historical process exits", exception)
        }
    }

    private val internalCemuDataFolder: String
        get() = internalFolder().resolve("data").toString()

    private val internalCemuUserFolder: String
        get() = internalFolder().toString()

    companion object {
        init {
            System.loadLibrary("CemuAndroid")
        }

        private var DefaultUncaughtExceptionHandler: Thread.UncaughtExceptionHandler? = null
        private const val INIT_TAG = "CemuInit"
    }
}

private class DebugDumpLifecycleCallbacks(
    private val application: Application,
) : Application.ActivityLifecycleCallbacks {
    override fun onActivityStarted(activity: Activity) {
        Log.i(LIFECYCLE_TAG, "${activity.javaClass.simpleName} started")
        DebugDumpService.start(application)
    }

    override fun onActivityCreated(activity: Activity, savedInstanceState: Bundle?) {
        Log.i(
            LIFECYCLE_TAG,
            "${activity.javaClass.simpleName} created savedState=${savedInstanceState != null}",
        )
    }

    override fun onActivityResumed(activity: Activity) {
        Log.i(LIFECYCLE_TAG, "${activity.javaClass.simpleName} resumed")
    }

    override fun onActivityPaused(activity: Activity) {
        Log.i(LIFECYCLE_TAG, "${activity.javaClass.simpleName} paused finishing=${activity.isFinishing}")
    }

    override fun onActivityStopped(activity: Activity) {
        Log.i(LIFECYCLE_TAG, "${activity.javaClass.simpleName} stopped finishing=${activity.isFinishing}")
    }

    override fun onActivitySaveInstanceState(activity: Activity, outState: Bundle) = Unit

    override fun onActivityDestroyed(activity: Activity) {
        Log.w(
            LIFECYCLE_TAG,
            "${activity.javaClass.simpleName} destroyed finishing=${activity.isFinishing} " +
                "changingConfig=${activity.isChangingConfigurations}",
        )
    }

    companion object {
        private const val LIFECYCLE_TAG = "CemuLifecycle"
    }
}

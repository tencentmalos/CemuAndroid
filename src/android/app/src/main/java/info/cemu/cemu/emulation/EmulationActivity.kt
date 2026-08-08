package info.cemu.cemu.emulation

import android.content.Context
import android.content.Intent
import android.os.Bundle
import android.util.Log
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.WindowManager
import androidx.activity.compose.setContent
import androidx.appcompat.app.AppCompatActivity
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.lifecycleScope
import androidx.lifecycle.repeatOnLifecycle
import info.cemu.cemu.BuildConfig
import info.cemu.cemu.common.android.inputevent.isFromPhysicalController
import info.cemu.cemu.common.gamelaunch.LastGameStore
import info.cemu.cemu.common.settings.AppSettingsStore
import info.cemu.cemu.common.ui.components.ActivityContent
import info.cemu.cemu.common.ui.localization.TranslatableContent
import info.cemu.cemu.emulation.input.ControllerCallbacks
import info.cemu.cemu.emulation.input.ControllerMotionHandler
import info.cemu.cemu.emulation.input.DeviceControllerCallbacks
import info.cemu.cemu.emulation.input.DeviceMotionHandler
import info.cemu.cemu.emulation.input.HotkeyManager
import info.cemu.cemu.emulation.input.InputHandler
import info.cemu.cemu.emulation.input.NativeInputDeviceListener
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.launch

private class InputDelegateManager(context: Context) {
    private val nativeInputDeviceListener = NativeInputDeviceListener(context)
    private val controllerCallbacks = ControllerCallbacks(context)
    private val controllerMotionHandler = ControllerMotionHandler(context)
    private val deviceControllerCallbacks = DeviceControllerCallbacks(context)
    private val deviceMotionHandler = DeviceMotionHandler(context)

    fun setDeviceMotionEnabled(isListening: Boolean) =
        deviceMotionHandler.setIsListening(isListening)

    fun registerAll() {
        nativeInputDeviceListener.register()
        controllerCallbacks.register()
        controllerMotionHandler.register()
        deviceControllerCallbacks.register()
    }

    fun unregisterAll() {
        nativeInputDeviceListener.unregister()
        controllerCallbacks.unregister()
        controllerMotionHandler.unregister()
        deviceControllerCallbacks.unregister()
    }

    fun onResume(rotation: Int) {
        registerAll()
        deviceMotionHandler.setDeviceRotation(rotation)
        deviceMotionHandler.resumeListening()
    }

    fun onPause() {
        unregisterAll()
        deviceMotionHandler.pauseListening()
    }
}

class EmulationActivity : AppCompatActivity() {
    private lateinit var inputManager: InputDelegateManager
    private var processInputEvents = true

    override fun onGenericMotionEvent(event: MotionEvent): Boolean {
        if (processInputEvents && InputHandler.onMotionEvent(event)) {
            return true
        }

        return super.onGenericMotionEvent(event)
    }

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        HotkeyManager.onKeyEvent(event)

        if (processInputEvents && InputHandler.onKeyEvent(event)) {
            return true
        }

        if (event.keyCode == KeyEvent.KEYCODE_BUTTON_MODE && event.isFromPhysicalController()) {
            return true
        }

        return super.dispatchKeyEvent(event)
    }

    private fun getGamePath(): String {
        val extras = intent.extras
        val data = intent.data
        var launchPath: String? = null

        if (extras != null) {
            launchPath = extras.getString(EXTRA_LAUNCH_PATH)
        }

        if (launchPath == null && data != null) {
            launchPath = data.toString()
        }

        if (launchPath == null) {
            throw RuntimeException("launchPath is null")
        }

        return launchPath
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        logWindowState("onCreate savedState=${savedInstanceState != null}")

        inputManager = InputDelegateManager(this)

        setupHotkeys()

        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        setFullscreen()

        val gamePath = getGamePath()
        Log.i(TAG, "launch requested path=${gamePath.substringAfterLast('/').takeLast(160)}")
        LastGameStore.record(this, gamePath)

        setContent {
            TranslatableContent {
                ActivityContent {
                    EmulationScreen(
                        gamePath = gamePath,
                        setMotionSensorEnabled = inputManager::setDeviceMotionEnabled,
                        onQuit = ::onQuit,
                        setInputListeningEnabled = { processInputEvents = it },
                    )
                }
            }
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        Log.i(
            TAG,
            "onNewIntent action=${intent.action} data=${intent.data?.lastPathSegment?.takeLast(160)} " +
                "hasLaunchPath=${intent.hasExtra(EXTRA_LAUNCH_PATH)}",
        )
    }

    override fun onStart() {
        super.onStart()
        logWindowState("onStart")
    }

    override fun onPause() {
        logWindowState("onPause")
        super.onPause()

        inputManager.onPause()
    }

    override fun onResume() {
        super.onResume()

        inputManager.onResume(display.rotation)
        logWindowState("onResume")
    }

    override fun onStop() {
        logWindowState("onStop")
        super.onStop()
    }

    override fun onDestroy() {
        logWindowState("onDestroy")
        super.onDestroy()
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        logWindowState("onWindowFocusChanged hasFocus=$hasFocus")
    }

    private fun setupHotkeys() {
        lifecycleScope.launch {
            repeatOnLifecycle(Lifecycle.State.STARTED) {
                AppSettingsStore.dataStore.data.map { it.hotkeySettings }
                    .distinctUntilChanged()
                    .collect { HotkeyManager.setHotkeyMappings(it) }
            }
        }
    }

    private fun setFullscreen() {
        WindowCompat.setDecorFitsSystemWindows(window, false)
        val controller = WindowInsetsControllerCompat(window, window.decorView)
        controller.systemBarsBehavior =
            WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        controller.hide(WindowInsetsCompat.Type.systemBars())
    }

    private fun onQuit() {
        Log.i(TAG, "onQuit finish requested")
        finish()
    }

    private fun logWindowState(event: String) {
        val configuration = resources.configuration
        val bounds = windowManager.currentWindowMetrics.bounds
        Log.i(
            TAG,
            "$event taskId=$taskId taskRoot=$isTaskRoot finishing=$isFinishing " +
                "changingConfig=$isChangingConfigurations lifecycle=${lifecycle.currentState} " +
                "orientation=${configuration.orientation} rotation=${display?.rotation} " +
                "bounds=${bounds.width()}x${bounds.height()}",
        )
    }

    companion object {
        private const val TAG = "CemuEmulation"
        const val EXTRA_LAUNCH_PATH: String = BuildConfig.APPLICATION_ID + ".LaunchPath"
    }
}

package info.cemu.cemu.emulation

import android.os.SystemClock
import android.util.Log
import android.view.SurfaceHolder
import androidx.datastore.core.DataStore
import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.viewModelScope
import androidx.lifecycle.viewmodel.CreationExtras
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import info.cemu.cemu.common.either.Either
import info.cemu.cemu.common.either.Error
import info.cemu.cemu.common.either.Success
import info.cemu.cemu.common.either.attemptWithContext
import info.cemu.cemu.common.either.bind
import info.cemu.cemu.common.settings.AppSettings
import info.cemu.cemu.common.settings.AppSettingsStore
import info.cemu.cemu.common.settings.InputOverlayRect
import info.cemu.cemu.common.settings.InputOverlaySettings
import info.cemu.cemu.common.settings.OverlayInputConfig
import info.cemu.cemu.nativeinterface.NativeEmulation
import info.cemu.cemu.nativeinterface.NativeEmulation.PrepareTitleResult
import info.cemu.cemu.nativeinterface.NativeException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.catch
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.onEach
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

data class SideMenuState(
    val isMotionEnabled: Boolean = false,
    val isTVReplacedWithPad: Boolean = false,
    val isPadVisible: Boolean = false,
    val isInputOverlayVisible: Boolean = false,
)

class ConditionFlags(
    var isMainConditionMet: Boolean = false, var isPadConditionMet: Boolean = false
) {
    fun get(isMain: Boolean): Boolean {
        return if (isMain) isMainConditionMet else isPadConditionMet
    }

    fun set(isMain: Boolean, value: Boolean) {
        if (isMain) {
            isMainConditionMet = value
        } else {
            isPadConditionMet = value
        }
    }
}

sealed interface NativeError {
    data class SurfaceCreationError(val message: String) : NativeError
    data class RendererInitializationError(val message: String) : NativeError

    object GameFilesNotFoundError : NativeError
    object NoDiscKeysError : NativeError
    object NoTitleTikError : NativeError
    data class UnknownTilePrepareError(val launchPath: String) : NativeError
    data class SystemInitializationError(val message: String) : NativeError

    object LaunchingTitleError : NativeError
}

class EmulationViewModel(
    private val launchPath: String,
    private val dataStore: DataStore<AppSettings> = AppSettingsStore.dataStore
) : ViewModel() {
    private val launchName = launchPath.substringAfterLast('/').takeLast(160)
    private val _emulationError = MutableStateFlow<NativeError?>(null)
    val emulationError = _emulationError.asStateFlow()

    private val _sideMenuState = MutableStateFlow(SideMenuState())
    val sideMenuState = _sideMenuState.asStateFlow()

    val isInputOverlayVisible =
        sideMenuState.map { it.isInputOverlayVisible }
            .stateIn(
                viewModelScope,
                SharingStarted.WhileSubscribed(5000),
                false,
            )

    init {
        Log.i(TAG, "ViewModel created launch=$launchName")
        viewModelScope.launch {
            try {
                val settings = dataStore.data.first()
                Log.i(
                    TAG,
                    "DataStore initial settings loaded inputOverlay=${settings.inputOverlaySettings.isOverlayEnabled}",
                )
                _sideMenuState.update {
                    it.copy(isInputOverlayVisible = settings.inputOverlaySettings.isOverlayEnabled)
                }
            } catch (exception: CancellationException) {
                throw exception
            } catch (exception: Exception) {
                Log.e(TAG, "DataStore initial settings failed", exception)
            }
        }
    }

    val inputOverlaySettings = dataStore.data.map { it.inputOverlaySettings }.stateIn(
        viewModelScope,
        SharingStarted.WhileSubscribed(5000),
        InputOverlaySettings(),
    )

    fun saveInputOverlayRectangles(inputOverlayRectMap: Map<OverlayInputConfig, InputOverlayRect>) {
        viewModelScope.launch {
            dataStore.updateData {
                val overlaySettings =
                    it.inputOverlaySettings.copy(inputOverlayRectMap = inputOverlayRectMap)

                it.copy(inputOverlaySettings = overlaySettings)
            }
        }
    }

    fun resetInputOverlayLayout() {
        viewModelScope.launch {
            dataStore.updateData {
                val overlaySettings =
                    it.inputOverlaySettings.copy(inputOverlayRectMap = emptyMap())

                it.copy(inputOverlaySettings = overlaySettings)
            }
        }
    }

    fun updateSideMenuState(sideMenuState: SideMenuState) {
        _sideMenuState.value = sideMenuState
    }

    val gamePadPosition = dataStore.data
        .map { it.emulationSettings.gamePadPosition }
        .distinctUntilChanged()
        .onEach { Log.i(TAG, "DataStore gamePadPosition=$it") }
        .catch { exception ->
            Log.e(TAG, "DataStore gamePadPosition stream failed", exception)
            throw exception
        }
        .stateIn(
            viewModelScope,
            SharingStarted.WhileSubscribed(5000),
            null,
        )

    val destroyedSurfaces = ConditionFlags()
    var setSurfaces = ConditionFlags()
    private var isShuttingDown = false

    private inner class CanvasSurfaceHolderCallback(val isMainCanvas: Boolean) :
        SurfaceHolder.Callback {

        override fun surfaceCreated(surfaceHolder: SurfaceHolder) {
            Log.i(
                TAG,
                "Native surfaceCreated canvas=${canvasName()} valid=${surfaceHolder.surface.isValid}",
            )
        }

        override fun surfaceChanged(
            surfaceHolder: SurfaceHolder,
            format: Int,
            width: Int,
            height: Int,
        ) {
            Log.i(
                TAG,
                "Native surfaceChanged canvas=${canvasName()} format=$format size=${width}x$height " +
                    "valid=${surfaceHolder.surface.isValid} alreadySet=${setSurfaces.get(isMainCanvas)}",
            )
            try {
                NativeEmulation.setSurfaceSize(width, height, isMainCanvas)

                if (setSurfaces.get(isMainCanvas)) {
                    Log.i(TAG, "Native surfaceChanged size-only canvas=${canvasName()}")
                    return
                }

                Log.i(TAG, "Native setSurface begin canvas=${canvasName()}")
                NativeEmulation.setSurface(surfaceHolder.surface, isMainCanvas)
                Log.i(TAG, "Native setSurface end canvas=${canvasName()}")
                val mainSurfaceWasDestroyed = destroyedSurfaces.get(isMain = true)

                if (mainSurfaceWasDestroyed && isMainCanvas) {
                    Log.i(TAG, "Native resumeTitle begin after main surface recreation")
                    NativeEmulation.resumeTitle()
                    Log.i(TAG, "Native resumeTitle end after main surface recreation")
                }

                setSurfaces.set(isMainCanvas, true)

                val padSurfaceWasSet = setSurfaces.get(isMain = false)
                if ((!isMainCanvas && !mainSurfaceWasDestroyed) || (isMainCanvas && padSurfaceWasSet)) {
                    Log.i(TAG, "Native initializeSurface begin canvas=pad")
                    NativeEmulation.initializeSurface(isMainCanvas = false)
                    Log.i(TAG, "Native initializeSurface end canvas=pad")
                }

                destroyedSurfaces.set(isMainCanvas, false)
            } catch (exception: NativeException) {
                Log.e(TAG, "Native surface setup failed canvas=${canvasName()}", exception)
                _emulationError.value =
                    NativeError.SurfaceCreationError(exception.message ?: "Unknown native error")
            }
        }

        override fun surfaceDestroyed(surfaceHolder: SurfaceHolder) {
            Log.w(
                TAG,
                "Native surfaceDestroyed canvas=${canvasName()} shuttingDown=$isShuttingDown " +
                    "mainSet=${setSurfaces.get(true)} padSet=${setSurfaces.get(false)}",
            )
            if (isShuttingDown) {
                setSurfaces.set(isMainCanvas, false)
                destroyedSurfaces.set(isMainCanvas, true)
                return
            }

            if (setSurfaces.get(isMain = false)) {
                Log.i(TAG, "Native clearPadSurface begin")
                NativeEmulation.clearPadSurface()
                Log.i(TAG, "Native clearPadSurface end")
                setSurfaces.set(isMain = false, false)
                destroyedSurfaces.set(isMain = false, true)
            }

            if (isMainCanvas) {
                Log.i(TAG, "Native pauseTitle begin after main surface destruction")
                NativeEmulation.pauseTitle()
                Log.i(TAG, "Native pauseTitle end after main surface destruction")

                setSurfaces.set(isMain = true, false)
                destroyedSurfaces.set(isMain = true, true)
            }
        }

        private fun canvasName() = if (isMainCanvas) "main" else "pad"
    }

    val mainHolderCallback: SurfaceHolder.Callback = CanvasSurfaceHolderCallback(true)
    val padHolderCallback: SurfaceHolder.Callback = CanvasSurfaceHolderCallback(false)

    private suspend fun initializeSystems(): Either<Unit, NativeError> {
        val startedAt = SystemClock.elapsedRealtime()
        Log.i(TAG, "stage initializeSystems begin")
        return attemptWithContext(Dispatchers.IO) {
            NativeEmulation.initializeSystems()
        }.fold(
            onSuccess = {
                Log.i(TAG, "stage initializeSystems end elapsedMs=${elapsedSince(startedAt)}")
                Success(Unit)
            },
            onError = { error ->
                Log.e(TAG, "stage initializeSystems failed elapsedMs=${elapsedSince(startedAt)} error=$error")
                Error(NativeError.SystemInitializationError(error))
            },
        )
    }

    private suspend fun initializeRenderer(): Either<Unit, NativeError> {
        val startedAt = SystemClock.elapsedRealtime()
        Log.i(TAG, "stage initializeRenderer begin")
        return attemptWithContext(Dispatchers.IO) {
            NativeEmulation.initializeRenderer()
            Log.i(TAG, "stage initializeRenderer renderer-created; initializeSurface main begin")
            NativeEmulation.initializeSurface(isMainCanvas = true)
        }.fold(
            onSuccess = {
                Log.i(TAG, "stage initializeRenderer end elapsedMs=${elapsedSince(startedAt)}")
                Success(Unit)
            },
            onError = { error ->
                Log.e(TAG, "stage initializeRenderer failed elapsedMs=${elapsedSince(startedAt)} error=$error")
                Error(NativeError.RendererInitializationError(error))
            },
        )
    }

    private suspend fun prepareTitle(): Either<Unit, NativeError> {
        val startedAt = SystemClock.elapsedRealtime()
        Log.i(TAG, "stage prepareTitle begin launch=$launchName")
        return attemptWithContext(Dispatchers.IO) { NativeEmulation.prepareTitle(launchPath) }
            .fold(
                onSuccess = { result ->
                    Log.i(
                        TAG,
                        "stage prepareTitle end elapsedMs=${elapsedSince(startedAt)} result=$result",
                    )
                    when (result) {
                        PrepareTitleResult.SUCCESSFUL -> Success(Unit)
                        PrepareTitleResult.ERROR_GAME_BASE_FILES_NOT_FOUND -> Error(NativeError.GameFilesNotFoundError)
                        PrepareTitleResult.ERROR_NO_DISC_KEY -> Error(NativeError.NoDiscKeysError)
                        PrepareTitleResult.ERROR_NO_TITLE_TIK -> Error(NativeError.NoTitleTikError)
                        else -> Error(NativeError.UnknownTilePrepareError(launchPath))
                    }
                },
                onError = { error ->
                    Log.e(
                        TAG,
                        "stage prepareTitle failed elapsedMs=${elapsedSince(startedAt)} error=$error",
                    )
                    Error(NativeError.UnknownTilePrepareError(launchPath))
                },
            )
    }

    private suspend fun launchTitle(): Either<Unit, NativeError> {
        val startedAt = SystemClock.elapsedRealtime()
        Log.i(TAG, "stage launchTitle begin")
        return attemptWithContext(Dispatchers.IO) { NativeEmulation.launchTitle() }
            .fold(
                onSuccess = {
                    Log.i(TAG, "stage launchTitle end elapsedMs=${elapsedSince(startedAt)}")
                    Success(Unit)
                },
                onError = { error ->
                    Log.e(TAG, "stage launchTitle failed elapsedMs=${elapsedSince(startedAt)} error=$error")
                    Error(NativeError.LaunchingTitleError)
                },
            )
    }

    private val _isEmulationInitialized = MutableStateFlow(false)
    val isEmulationInitialized = _isEmulationInitialized.asStateFlow()
    private var emulationInitializationJob: Job? = null
    fun initializeEmulation() {
        Log.i(
            TAG,
            "initializeEmulation requested initialized=${_isEmulationInitialized.value} " +
                "job=${emulationInitializationJob?.let { "active=${it.isActive} completed=${it.isCompleted}" }}",
        )
        if (_isEmulationInitialized.value || emulationInitializationJob != null) {
            Log.w(TAG, "initializeEmulation ignored because it already started")
            return
        }

        emulationInitializationJob = viewModelScope.launch {
            val startedAt = SystemClock.elapsedRealtime()
            Log.i(TAG, "initialization job started")
            try {
                val result = prepareTitle()
                    .bind { initializeSystems() }
                    .bind { initializeRenderer() }
                    .bind { launchTitle() }

                result.fold(
                    onSuccess = {
                        Log.i(TAG, "initialization pipeline succeeded elapsedMs=${elapsedSince(startedAt)}")
                    },
                    onError = { error ->
                        Log.e(TAG, "initialization pipeline stopped elapsedMs=${elapsedSince(startedAt)} error=$error")
                        _emulationError.value = error
                    },
                )

                _isEmulationInitialized.value = true
                Log.i(TAG, "initialization dialog released")
            } catch (exception: CancellationException) {
                Log.w(TAG, "initialization job cancelled elapsedMs=${elapsedSince(startedAt)}")
                throw exception
            } catch (throwable: Throwable) {
                Log.e(TAG, "initialization job crashed elapsedMs=${elapsedSince(startedAt)}", throwable)
                throw throwable
            }
        }
    }

    fun stopEmulation(onStopped: () -> Unit) {
        Log.i(TAG, "stopEmulation requested shuttingDown=$isShuttingDown")
        if (isShuttingDown) {
            Log.w(TAG, "stopEmulation ignored because shutdown already started")
            return
        }

        isShuttingDown = true
        viewModelScope.launch {
            try {
                Log.i(TAG, "stopEmulation cancel initialization begin")
                emulationInitializationJob?.cancelAndJoin()
                Log.i(TAG, "stopEmulation native shutdown begin")
                NativeEmulation.stopEmulation()
                Log.i(TAG, "stopEmulation native shutdown end")
            } finally {
                Log.i(TAG, "stopEmulation invoking Activity finish")
                onStopped()
            }
        }
    }

    override fun onCleared() {
        Log.w(
            TAG,
            "ViewModel cleared initialized=${_isEmulationInitialized.value} shuttingDown=$isShuttingDown " +
                "job=${emulationInitializationJob?.let { "active=${it.isActive} completed=${it.isCompleted}" }}",
        )
        super.onCleared()
    }

    private fun elapsedSince(startedAt: Long) = SystemClock.elapsedRealtime() - startedAt

    companion object {
        private const val TAG = "CemuEmulation"
        val LAUNCH_PATH_KEY = object : CreationExtras.Key<String> {}
        val Factory: ViewModelProvider.Factory = viewModelFactory {
            initializer {
                EmulationViewModel(
                    this[LAUNCH_PATH_KEY] as String
                )
            }
        }
    }
}

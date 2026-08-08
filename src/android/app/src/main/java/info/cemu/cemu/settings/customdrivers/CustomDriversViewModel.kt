@file:OptIn(ExperimentalPathApi::class, ExperimentalUuidApi::class)

package info.cemu.cemu.settings.customdrivers

import android.content.Context
import android.net.Uri
import android.os.Build
import android.util.Log
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import info.cemu.cemu.common.customdrivers.DriverMetadata
import info.cemu.cemu.common.customdrivers.META_FILE_NAME
import info.cemu.cemu.common.customdrivers.SUPPORTED_SCHEMA_VERSION
import info.cemu.cemu.common.customdrivers.getCustomDriversDir
import info.cemu.cemu.common.customdrivers.parseInstalledDrivers
import info.cemu.cemu.common.io.decodeJsonFromFile
import info.cemu.cemu.common.io.unzip
import info.cemu.cemu.nativeinterface.NativeActiveSettings
import info.cemu.cemu.nativeinterface.NativeSettings
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.InputStream
import kotlin.io.path.ExperimentalPathApi
import kotlin.io.path.Path
import kotlin.io.path.createDirectories
import kotlin.io.path.deleteRecursively
import kotlin.io.path.exists
import kotlin.io.path.moveTo
import kotlin.uuid.ExperimentalUuidApi
import kotlin.uuid.Uuid

data class Driver(
    val path: String,
    val metadata: DriverMetadata,
    val selected: Boolean = false,
)

enum class DriverInstallStatus {
    Installed,
    AlreadyInstalled,
    ErrorReadingArchive,
    InvalidArchive,
    InvalidMetadata,
    UnsupportedAndroidVersion,
    UnsupportedSchema,
    InvalidLibrary,
    StorageError,
}

sealed interface RemoteDriverOperation {
    data object Idle : RemoteDriverOperation
    data object Fetching : RemoteDriverOperation
    data class Downloading(val driverName: String) : RemoteDriverOperation
    data class Installing(val driverName: String) : RemoteDriverOperation
}

sealed interface CustomDriverEvent {
    data class DriverInstalled(val driverName: String) : CustomDriverEvent
    data class DriverInstallFailed(val status: DriverInstallStatus) : CustomDriverEvent
    data class Error(val message: String) : CustomDriverEvent
}

private data class DriverInstallResult(
    val status: DriverInstallStatus,
    val driver: Driver? = null,
)

class CustomDriversViewModel(
    private val customDriverDownloader: CustomDriverDownloader = CustomDriverDownloader(),
) : ViewModel() {
    private val selectedDriverPath = MutableStateFlow(NativeSettings.getCustomDriverPath())
    val isSystemDriverSelected = selectedDriverPath.map { it == null }.stateIn(
        viewModelScope,
        SharingStarted.WhileSubscribed(5000),
        false
    )

    private val _installedDrivers = MutableStateFlow<List<Driver>>(emptyList())
    val installedDrivers = _installedDrivers.asStateFlow()

    private val _remoteDrivers = MutableStateFlow<List<RemoteDriver>>(emptyList())
    val remoteDrivers = _remoteDrivers.asStateFlow()

    private val _remoteDriverOperation = MutableStateFlow<RemoteDriverOperation>(RemoteDriverOperation.Idle)
    val remoteDriverOperation = _remoteDriverOperation.asStateFlow()

    private val _events = MutableSharedFlow<CustomDriverEvent>(extraBufferCapacity = 1)
    val events = _events

    private var remoteDriverJob: Job? = null

    init {
        viewModelScope.launch {
            val selectedDriver = selectedDriverPath.value

            _installedDrivers.value = parseInstalledDrivers().map {
                Driver(
                    it.path,
                    it.metadata,
                    selected = selectedDriver == it.path,
                )
            }
        }
    }

    private val _isDriverInstallInProgress = MutableStateFlow(false)
    val isDriverInstallInProgress = _isDriverInstallInProgress.asStateFlow()

    fun installDriver(
        context: Context,
        driverZipUri: Uri,
        onInstallFinished: (DriverInstallStatus) -> Unit,
    ) {
        _isDriverInstallInProgress.value = true
        viewModelScope.launch(Dispatchers.IO) {
            val result = try {
                context.contentResolver.openInputStream(driverZipUri)?.use { installDriver(it) }
                    ?: DriverInstallResult(DriverInstallStatus.ErrorReadingArchive)
            } catch (exception: Exception) {
                Log.e(TAG, "Unable to read custom driver URI $driverZipUri", exception)
                DriverInstallResult(DriverInstallStatus.ErrorReadingArchive)
            } finally {
                _isDriverInstallInProgress.value = false
            }
            onInstallFinished(result.status)
        }
    }

    private fun installDriver(stream: InputStream): DriverInstallResult {
        val tempDir =
            Path(NativeActiveSettings.getUserDataPath()).resolve(Uuid.random().toString())

        try {
            tempDir.createDirectories()
            try {
                unzip(stream, tempDir)
            } catch (exception: Exception) {
                Log.e(TAG, "Unable to extract custom driver archive", exception)
                return DriverInstallResult(DriverInstallStatus.InvalidArchive)
            }

            val metadata =
                decodeJsonFromFile<DriverMetadata>(tempDir.resolve(META_FILE_NAME).toFile())
            if (metadata == null)
                return DriverInstallResult(DriverInstallStatus.InvalidMetadata)
            if (metadata.minApi > Build.VERSION.SDK_INT)
                return DriverInstallResult(DriverInstallStatus.UnsupportedAndroidVersion)
            if (metadata.schemaVersion != SUPPORTED_SCHEMA_VERSION)
                return DriverInstallResult(DriverInstallStatus.UnsupportedSchema)
            if (metadata.libraryName.contains('/')
                || metadata.libraryName.contains('\\')
                || !metadata.libraryName.endsWith(".so")
                || !tempDir.resolve(metadata.libraryName).exists())
                return DriverInstallResult(DriverInstallStatus.InvalidLibrary)

            if (_installedDrivers.value.any { it.metadata == metadata })
                return DriverInstallResult(DriverInstallStatus.AlreadyInstalled)

            val customDriversDir = getCustomDriversDir()
            customDriversDir.createDirectories()
            val driverPath = tempDir.moveTo(customDriversDir.resolve(tempDir.fileName))
            val driver = Driver(
                metadata = metadata,
                path = driverPath.toString(),
            )
            _installedDrivers.value = _installedDrivers.value.toMutableList().apply {
                add(driver)
                sortBy { it.metadata.name }
            }
            return DriverInstallResult(DriverInstallStatus.Installed, driver)
        } catch (exception: Exception) {
            Log.e(TAG, "Unable to store custom driver", exception)
            return DriverInstallResult(DriverInstallStatus.StorageError)
        } finally {
            runCatching {
                if (tempDir.exists())
                    tempDir.deleteRecursively()
            }.onFailure { Log.w(TAG, "Unable to remove temporary driver directory", it) }
        }
    }

    fun refreshRemoteDrivers() {
        if (remoteDriverJob?.isActive == true)
            return
        remoteDriverJob = viewModelScope.launch {
            _remoteDriverOperation.value = RemoteDriverOperation.Fetching
            try {
                _remoteDrivers.value = withContext(Dispatchers.IO) {
                    customDriverDownloader.fetchDrivers()
                }
                if (_remoteDrivers.value.isEmpty())
                    _events.emit(CustomDriverEvent.Error("No compatible driver downloads found"))
            } catch (exception: Exception) {
                _events.emit(CustomDriverEvent.Error(exception.message ?: "Failed to fetch drivers"))
            } finally {
                _remoteDriverOperation.value = RemoteDriverOperation.Idle
            }
        }
    }

    fun downloadAndInstall(driver: RemoteDriver) {
        if (remoteDriverJob?.isActive == true)
            return
        remoteDriverJob = viewModelScope.launch {
            try {
                _remoteDriverOperation.value = RemoteDriverOperation.Downloading(driver.releaseName)
                val bytes = withContext(Dispatchers.IO) {
                    customDriverDownloader.download(driver)
                }
                _remoteDriverOperation.value = RemoteDriverOperation.Installing(driver.releaseName)
                val result = withContext(Dispatchers.IO) {
                    bytes.inputStream().use { installDriver(it) }
                }
                when (result.status) {
                    DriverInstallStatus.Installed -> {
                        result.driver?.let(::setDriverSelected)
                        _events.emit(CustomDriverEvent.DriverInstalled(driver.releaseName))
                    }

                    DriverInstallStatus.AlreadyInstalled -> {
                        val installedDriver = _installedDrivers.value.firstOrNull {
                            it.metadata.name == driver.releaseName ||
                                    it.metadata.packageVersion in driver.releaseName
                        }
                        installedDriver?.let(::setDriverSelected)
                        _events.emit(CustomDriverEvent.Error("Driver is already installed"))
                    }

                    else ->
                        _events.emit(CustomDriverEvent.DriverInstallFailed(result.status))
                }
            } catch (exception: Exception) {
                _events.emit(CustomDriverEvent.Error(exception.message ?: "Failed to download driver"))
            } finally {
                _remoteDriverOperation.value = RemoteDriverOperation.Idle
            }
        }
    }

    fun deleteDriver(driver: Driver) {
        if (!_installedDrivers.value.any { it == driver })
            return

        _installedDrivers.value -= driver
        if (selectedDriverPath.value == driver.path) {
            selectedDriverPath.value = null
            NativeSettings.setCustomDriverPath(null)
        }

        viewModelScope.launch(Dispatchers.IO) {
            Path(driver.path).toFile().deleteRecursively()
        }
    }

    fun setSystemDriverSelected() {
        if (selectedDriverPath.value == null)
            return

        val installedDrivers = _installedDrivers.value.toMutableList()
        val oldSelectedDriverIndex = installedDrivers.indexOfFirst { it.selected }
        if (oldSelectedDriverIndex != -1) {
            installedDrivers[oldSelectedDriverIndex] =
                installedDrivers[oldSelectedDriverIndex].copy(selected = false)
            _installedDrivers.value = installedDrivers
        }

        selectedDriverPath.value = null
        NativeSettings.setCustomDriverPath(null)
    }

    fun setDriverSelected(driver: Driver) {
        if (selectedDriverPath.value == driver.path)
            return

        val installedDrivers = _installedDrivers.value.toMutableList()

        val oldSelectedDriverIndex = installedDrivers.indexOfFirst { it.selected }
        if (oldSelectedDriverIndex != -1)
            installedDrivers[oldSelectedDriverIndex] =
                installedDrivers[oldSelectedDriverIndex].copy(selected = false)

        val newSelectedDriverIndex = installedDrivers.indexOf(driver)
        if (newSelectedDriverIndex == -1)
            return
        installedDrivers[newSelectedDriverIndex] = driver.copy(selected = true)

        _installedDrivers.value = installedDrivers

        NativeSettings.setCustomDriverPath(driver.path)
        selectedDriverPath.value = driver.path
    }

    companion object {
        private const val TAG = "CemuCustomDrivers"
    }
}

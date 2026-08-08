package info.cemu.cemu.settings.customdrivers

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.animation.animateContentSize
import androidx.compose.foundation.basicMarquee
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Card
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.RadioButton
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.rotate
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.viewmodel.compose.viewModel
import info.cemu.cemu.R
import info.cemu.cemu.common.customdrivers.DriverMetadata
import info.cemu.cemu.common.ui.components.ScreenContentLazy
import info.cemu.cemu.common.ui.localization.tr
import kotlinx.coroutines.launch

@Composable
fun CustomDriversScreen(
    navigateBack: () -> Unit,
    customDriversViewModel: CustomDriversViewModel = viewModel(),
) {
    val coroutineScope = rememberCoroutineScope()
    val snackbarHostState = remember { SnackbarHostState() }
    val installedDrivers by customDriversViewModel.installedDrivers.collectAsState()
    val isSystemDriverSelected by customDriversViewModel.isSystemDriverSelected.collectAsState()
    val isDriverInstallInProgress by customDriversViewModel.isDriverInstallInProgress.collectAsState()
    val remoteDrivers by customDriversViewModel.remoteDrivers.collectAsState()
    val remoteDriverOperation by customDriversViewModel.remoteDriverOperation.collectAsState()
    val context = LocalContext.current

    LaunchedEffect(customDriversViewModel) {
        customDriversViewModel.events.collect { event ->
            val message = when (event) {
                is CustomDriverEvent.DriverInstalled ->
                    tr("Driver installed and selected. Restart Cemu to use it")

                is CustomDriverEvent.DriverInstallFailed ->
                    driverInstallStatusToString(event.status)

                is CustomDriverEvent.Error -> event.message
            }
            snackbarHostState.currentSnackbarData?.dismiss()
            snackbarHostState.showSnackbar(message)
        }
    }

    LaunchedEffect(customDriversViewModel) {
        if (remoteDrivers.isEmpty())
            customDriversViewModel.refreshRemoteDrivers()
    }

    val customDriversInstallLauncher =
        rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
            if (uri == null) return@rememberLauncherForActivityResult

            customDriversViewModel.installDriver(context, uri) { installStatus ->
                val message = when (installStatus) {
                    DriverInstallStatus.AlreadyInstalled -> tr("Driver already installed")
                    DriverInstallStatus.Installed -> tr("Driver installed successfully")
                    else -> driverInstallStatusToString(installStatus)
                }

                coroutineScope.launch {
                    snackbarHostState.currentSnackbarData?.dismiss()
                    snackbarHostState.showSnackbar(message)
                }
            }
        }

    ScreenContentLazy(
        snackbarHost = { SnackbarHost(hostState = snackbarHostState) },
        appBarText = tr("Custom drivers"),
        navigateBack = navigateBack,
        actions = {
            IconButton(
                enabled = remoteDriverOperation == RemoteDriverOperation.Idle,
                onClick = customDriversViewModel::refreshRemoteDrivers,
            ) {
                Icon(
                    painter = painterResource(R.drawable.ic_refresh),
                    contentDescription = tr("Refresh driver downloads")
                )
            }
            IconButton(onClick = { customDriversInstallLauncher.launch(arrayOf("application/zip")) }) {
                Icon(
                    painter = painterResource(R.drawable.ic_add),
                    contentDescription = null
                )
            }
        },
    ) {
        item {
            SystemDriverListItem(
                selected = isSystemDriverSelected,
                onSelect = customDriversViewModel::setSystemDriverSelected
            )
        }
        item {
            Text(
                modifier = Modifier.padding(start = 12.dp, end = 12.dp, top = 12.dp),
                text = tr("Download drivers"),
                fontSize = 18.sp,
                fontWeight = FontWeight.Bold,
            )
            Text(
                modifier = Modifier.padding(horizontal = 12.dp, vertical = 4.dp),
                text = tr("Custom drivers take effect after restarting Cemu. System driver always remains available"),
                fontSize = 14.sp,
            )
        }
        if (remoteDriverOperation == RemoteDriverOperation.Fetching && remoteDrivers.isEmpty()) {
            item {
                LinearProgressIndicator(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(12.dp)
                )
            }
        }
        items(
            items = remoteDrivers,
            key = { "${it.repository}/${it.assetName}" },
        ) { driver ->
            RemoteDriverListItem(
                driver = driver,
                enabled = remoteDriverOperation == RemoteDriverOperation.Idle,
                onDownload = { customDriversViewModel.downloadAndInstall(driver) },
            )
        }
        items(installedDrivers) {
            CustomDriverListItem(
                driver = it,
                onDelete = { customDriversViewModel.deleteDriver(it) },
                onSelect = { customDriversViewModel.setDriverSelected(it) }
            )
        }
    }

    if (isDriverInstallInProgress)
        DriverInstallProgressDialog()

    when (val operation = remoteDriverOperation) {
        is RemoteDriverOperation.Downloading ->
            RemoteDriverProgressDialog(tr("Downloading driver"), operation.driverName)

        is RemoteDriverOperation.Installing ->
            RemoteDriverProgressDialog(tr("Installing driver"), operation.driverName)

        else -> Unit
    }
}

private fun driverInstallStatusToString(status: DriverInstallStatus): String = when (status) {
    DriverInstallStatus.Installed -> tr("Driver installed successfully")
    DriverInstallStatus.AlreadyInstalled -> tr("Driver already installed")
    DriverInstallStatus.ErrorReadingArchive -> tr("Unable to read the selected driver archive")
    DriverInstallStatus.InvalidArchive -> tr("The selected driver is not a valid ZIP archive")
    DriverInstallStatus.InvalidMetadata -> tr("The driver metadata is missing or invalid")
    DriverInstallStatus.UnsupportedAndroidVersion -> tr("The driver does not support this Android version")
    DriverInstallStatus.UnsupportedSchema -> tr("The driver package format is not supported")
    DriverInstallStatus.InvalidLibrary -> tr("The driver Vulkan library is missing or invalid")
    DriverInstallStatus.StorageError -> tr("Cemu could not store the driver in its app data directory")
}

@Composable
private fun RemoteDriverListItem(
    driver: RemoteDriver,
    enabled: Boolean,
    onDownload: () -> Unit,
) {
    Card(
        modifier = Modifier
            .fillMaxWidth()
            .padding(8.dp)
    ) {
        Row(
            modifier = Modifier.padding(start = 12.dp, top = 8.dp, bottom = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Column(modifier = Modifier.weight(1.0f)) {
                Text(
                    text = driver.releaseName,
                    fontSize = 17.sp,
                    fontWeight = FontWeight.Bold,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
                Text(
                    text = if (driver.recommendedForAdreno8xx)
                        "${driver.sourceName} · ${tr("Recommended for Adreno 8xx")}"
                    else
                        driver.sourceName,
                    fontSize = 14.sp,
                )
                Text(
                    text = "${driver.assetName} · ${driver.publishedAt.take(10)}",
                    fontSize = 12.sp,
                )
            }
            IconButton(enabled = enabled, onClick = onDownload) {
                Icon(
                    painter = painterResource(R.drawable.ic_download),
                    contentDescription = tr("Download driver")
                )
            }
        }
    }
}

@Composable
private fun RemoteDriverProgressDialog(title: String, driverName: String) {
    AlertDialog(
        title = { Text(title) },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(driverName)
                LinearProgressIndicator(modifier = Modifier.fillMaxWidth())
            }
        },
        onDismissRequest = {},
        confirmButton = {},
        dismissButton = {},
    )
}

@Composable
private fun DriverInstallProgressDialog() {
    AlertDialog(
        title = {
            Text(tr("Installing"))
        },
        text = {
            Column(
                modifier = Modifier.padding(8.dp),
                verticalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                Text(tr("Installing driver in progress"))
                LinearProgressIndicator(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(vertical = 8.dp)
                )
            }
        },
        onDismissRequest = {},
        confirmButton = {},
        dismissButton = {}
    )
}

@Composable
private fun SystemDriverListItem(selected: Boolean, onSelect: () -> Unit) {
    DriverListItem(
        driverLabel = tr("System driver"),
        selected = selected,
        onSelect = onSelect
    )
}

@Composable
private fun CustomDriverListItem(driver: Driver, onDelete: () -> Unit, onSelect: () -> Unit) {
    var showDriverInfo by remember { mutableStateOf(false) }

    DriverListItem(
        driverLabel = driver.metadata.name,
        selected = driver.selected,
        onSelect = onSelect,
        labelExtraContent = {
            IconButton(onClick = { showDriverInfo = !showDriverInfo }) {
                Icon(
                    modifier = Modifier.rotate(if (showDriverInfo) 180f else 0f),
                    painter = painterResource(R.drawable.ic_arrow_drop_down),
                    contentDescription = null
                )
            }
            IconButton(onClick = onDelete) {
                Icon(
                    painter = painterResource(R.drawable.ic_delete),
                    contentDescription = null
                )
            }
        }
    ) {
        if (showDriverInfo) {
            DriverMetadataInfo(driver.metadata)
        }
    }
}

@Composable
private fun DriverListItem(
    driverLabel: String,
    selected: Boolean,
    onSelect: () -> Unit,
    labelExtraContent: @Composable RowScope.() -> Unit = {},
    content: @Composable ColumnScope.() -> Unit = {},
) {
    Card(
        modifier = Modifier
            .animateContentSize()
            .fillMaxWidth()
            .padding(8.dp),
        onClick = onSelect
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            RadioButton(selected = selected, onClick = onSelect)
            Text(
                modifier = Modifier
                    .padding(horizontal = 4.dp)
                    .basicMarquee(iterations = Int.MAX_VALUE)
                    .weight(1.0f),
                text = driverLabel,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                fontSize = 18.sp,
                fontWeight = FontWeight.Bold,
            )
            labelExtraContent()
        }
        content()
    }
}

@Composable
private fun DriverMetadataInfo(metadata: DriverMetadata) {
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .padding(8.dp)
    ) {
        DriverMetadataInfo(tr("Description"), metadata.description)
        DriverMetadataInfo(tr("Author"), metadata.author)
        DriverMetadataInfo(tr("Package version"), metadata.packageVersion)
        DriverMetadataInfo(tr("Vendor"), metadata.vendor)
        DriverMetadataInfo(tr("Driver version"), metadata.driverVersion)
        DriverMetadataInfo(tr("Min api"), metadata.minApi)
    }
}

@Composable
private fun <T> DriverMetadataInfo(label: String, info: T) {
    Text(
        modifier = Modifier.padding(start = 8.dp, end = 8.dp, top = 2.dp),
        fontSize = 16.sp,
        fontWeight = FontWeight.Medium,
        text = label
    )
    Text(
        modifier = Modifier.padding(start = 8.dp, end = 8.dp, bottom = 2.dp),
        fontSize = 14.sp,
        text = info.toString(),
    )
}

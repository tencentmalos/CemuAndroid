package info.cemu.cemu.settings.input.controller.emulatedcontroller

import android.view.KeyEvent
import androidx.compose.foundation.background
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.key.KeyEventType
import androidx.compose.ui.input.key.onPreviewKeyEvent
import androidx.compose.ui.input.key.type
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import info.cemu.cemu.common.android.inputevent.isFromPhysicalController
import info.cemu.cemu.common.ui.components.Header

@Composable
fun InputItemsGroup(
    groupName: String,
    inputIds: List<Int>,
    inputIdToString: (Int) -> String,
    onInputClick: (String, Int) -> Unit,
    onInputLongClick: (Int) -> Unit,
    controlsMapping: Map<Int, String>,
) {
    Header(groupName)
    inputIds.forEach {
        val buttonName = inputIdToString(it)
        InputItem(
            buttonName = buttonName,
            mapping = controlsMapping[it],
            onClick = { onInputClick(buttonName, it) },
            onLongClick = { onInputLongClick(it) },
        )
    }
}

@Composable
fun InputItem(
    buttonName: String,
    mapping: String?,
    onClick: () -> Unit,
    onLongClick: () -> Unit,
) {
    var isFocused by remember { mutableStateOf(false) }

    Column(
        modifier = Modifier
            .fillMaxWidth()
            .onFocusChanged { isFocused = it.isFocused }
            .onPreviewKeyEvent { event ->
                val nativeEvent = event.nativeKeyEvent
                if (nativeEvent.keyCode != KeyEvent.KEYCODE_BUTTON_A ||
                    !nativeEvent.isFromPhysicalController()
                ) {
                    return@onPreviewKeyEvent false
                }

                if (event.type == KeyEventType.KeyUp) {
                    onClick()
                }
                true
            }
            .combinedClickable(
                onClick = onClick,
                onLongClick = onLongClick,
            )
            .background(
                color = if (isFocused) {
                    MaterialTheme.colorScheme.secondaryContainer
                } else {
                    Color.Transparent
                },
                shape = MaterialTheme.shapes.small,
            )
            .padding(8.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        Text(
            text = buttonName,
            fontSize = 18.sp,
        )
        Text(
            text = mapping ?: "",
            fontSize = 16.sp
        )
    }
}

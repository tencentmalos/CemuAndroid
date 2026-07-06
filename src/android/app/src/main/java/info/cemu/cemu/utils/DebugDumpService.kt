package info.cemu.cemu.utils

import android.app.Service
import android.content.Context
import android.content.Intent
import android.os.IBinder
import info.cemu.cemu.nativeinterface.NativeDebugDump
import java.io.FileDescriptor
import java.io.PrintWriter

class DebugDumpService : Service() {
    override fun onBind(intent: Intent?): IBinder? = null

    override fun dump(fd: FileDescriptor?, writer: PrintWriter, args: Array<out String>?) {
        val dumpArgs = args?.toList().orEmpty()
        val request = dumpArgs.firstOrNull() ?: "help"
        writer.print(NativeDebugDump.getDebugDump(request, dumpArgs.drop(1).toTypedArray()))
    }

    companion object {
        fun start(context: Context) {
            val appContext = context.applicationContext
            appContext.startService(Intent(appContext, DebugDumpService::class.java))
        }
    }
}

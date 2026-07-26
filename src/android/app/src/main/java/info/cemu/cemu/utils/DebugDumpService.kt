package info.cemu.cemu.utils

import android.app.Service
import android.content.Context
import android.content.Intent
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import info.cemu.cemu.nativeinterface.NativeDebugDump
import java.io.FileDescriptor
import java.io.PrintWriter

class DebugDumpService : Service() {
    private val targetHandler = Handler(Looper.getMainLooper())
    private val requestExecutor = DebugDumpRequestExecutor(
        postToTarget = targetHandler::post,
        isTargetThread = { Looper.myLooper() == targetHandler.looper },
    )

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int = START_STICKY

    override fun dump(fd: FileDescriptor?, writer: PrintWriter, args: Array<out String>?) {
        val dumpArgs = args?.toList().orEmpty()
        val request = dumpArgs.firstOrNull() ?: "help"
        writer.print(
            requestExecutor.execute {
                NativeDebugDump.getDebugDump(request, dumpArgs.drop(1).toTypedArray())
            }
        )
    }

    companion object {
        fun start(context: Context) {
            val appContext = context.applicationContext
            appContext.startService(Intent(appContext, DebugDumpService::class.java))
        }
    }
}

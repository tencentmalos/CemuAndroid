package info.cemu.cemu.nativeinterface

object NativeDebugDump {
    @JvmStatic
    external fun initialize()

    @JvmStatic
    external fun getDebugDump(request: String, args: Array<String>): String
}

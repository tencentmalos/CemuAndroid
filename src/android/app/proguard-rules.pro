-dontobfuscate

# DebugDumpService is intentionally retained in release builds.
-keep class info.cemu.cemu.utils.DebugDumpService { *; }
-keep class info.cemu.cemu.nativeinterface.NativeDebugDump { *; }

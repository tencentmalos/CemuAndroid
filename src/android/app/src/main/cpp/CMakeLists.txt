add_library(CemuAndroid SHARED
        AndroidFilesystemCallbacks.cpp
        AndroidInputHelpers.cpp
        CompressTitleCallbacks.cpp
        GameTitleLoader.cpp
        Image.cpp
        JNIUtils.cpp
        NativeAccount.cpp
        NativeActiveSettings.cpp
        NativeEmulatedUSBDevices.cpp
        NativeEmulation.cpp
        NativeGameTitles.cpp
        NativeGraphicPacks.cpp
        NativeInput.cpp
        NativeLib.cpp
        NativeLocalization.cpp
        NativeLogging.cpp
        NativeDebugDump.cpp
        NativeSettings.cpp
        NativeSwkbd.cpp
        WuaConverter.cpp
)

cemu_use_precompiled_header(CemuAndroid)

target_include_directories(CemuAndroid PRIVATE
        "${CMAKE_SOURCE_DIR}/dependencies/foundation/basic/underlying/core/public"
)

target_link_libraries(CemuAndroid PRIVATE
        -landroid
        -llog
        CemuCommon
        CemuAudio
        CemuComponents
        CemuCafe
        CemuBin
        CemuGui
        CemuInput
        CemuUtil
        spatial::foundation_debugbus
        ZArchive::zarchive
        stb
)

cmake_minimum_required(VERSION 3.25)

option(MACOS_BUNDLE "The executable when built on macOS will be created as an application bundle" OFF)
option(ALLOW_PORTABLE "Allow Cemu to be run in portable mode" ON)

# used by CI script to set version:
set(EMULATOR_VERSION_MAJOR "0" CACHE STRING "")
set(EMULATOR_VERSION_MINOR "0" CACHE STRING "")
set(EMULATOR_VERSION_PATCH "0" CACHE STRING "")

set(THREADS_PREFER_PTHREAD_FLAG true)

execute_process(
		COMMAND git log --format=%h -1
		WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
		OUTPUT_VARIABLE GIT_HASH
		OUTPUT_STRIP_TRAILING_WHITESPACE
)
add_definitions(-DEMULATOR_HASH=${GIT_HASH})

project(Cemu VERSION 2.0.0 LANGUAGES C CXX ASM)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (ANDROID AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
	# NDK 29 marks several standard type traits as non-specializable. The pinned
	# foundation half type predates that diagnostic and remains ABI-compatible.
	add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-Wno-invalid-specialization>)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_definitions($<$<CONFIG:Debug>:CEMU_DEBUG_ASSERT>) # if build type is debug, set CEMU_DEBUG_ASSERT

add_definitions(-DEMULATOR_VERSION_MAJOR=${EMULATOR_VERSION_MAJOR})
add_definitions(-DEMULATOR_VERSION_MINOR=${EMULATOR_VERSION_MINOR})
add_definitions(-DEMULATOR_VERSION_PATCH=${EMULATOR_VERSION_PATCH})

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# Link-time optimization is useful for shipping builds, but slows down the
# RelWithDebInfo builds used during development.
if (CMAKE_BUILD_TYPE STREQUAL "Release")
	set(DEFAULT_ENABLE_LTO ON)
else()
	set(DEFAULT_ENABLE_LTO OFF)
endif()
option(ENABLE_LTO "Enable link-time optimization" ${DEFAULT_ENABLE_LTO})
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ${ENABLE_LTO})

option(CEMU_USE_PRECOMPILED_HEADERS "Use precompiled headers across the complete build graph" OFF)
if (CEMU_USE_PRECOMPILED_HEADERS)
	set(CMAKE_DISABLE_PRECOMPILE_HEADERS OFF)
else()
	set(CMAKE_DISABLE_PRECOMPILE_HEADERS ON)
endif()

option(CEMU_USE_CCACHE "Use ccache as a compiler launcher when available" ON)
if (CEMU_USE_CCACHE)
	if (NOT "$ENV{NDK_CCACHE}" STREQUAL "")
		set(CEMU_CCACHE_EXECUTABLE "$ENV{NDK_CCACHE}")
	else()
		find_program(CEMU_CCACHE_EXECUTABLE ccache)
	endif()

	if (CEMU_CCACHE_EXECUTABLE)
		execute_process(
			COMMAND "${CEMU_CCACHE_EXECUTABLE}" --version
			RESULT_VARIABLE CEMU_CCACHE_VERSION_RESULT
			OUTPUT_QUIET
			ERROR_QUIET
			TIMEOUT 10
		)
		if (CEMU_CCACHE_VERSION_RESULT EQUAL 0)
			execute_process(
				COMMAND "${CEMU_CCACHE_EXECUTABLE}" --get-config sloppiness
				RESULT_VARIABLE CEMU_CCACHE_SLOPPINESS_RESULT
				OUTPUT_VARIABLE CEMU_CCACHE_SLOPPINESS
				OUTPUT_STRIP_TRAILING_WHITESPACE
				ERROR_QUIET
				TIMEOUT 10
			)
			if (CEMU_CCACHE_SLOPPINESS_RESULT EQUAL 0
				AND CEMU_CCACHE_SLOPPINESS MATCHES "(^|,)pch_defines(,|$)")
				message(FATAL_ERROR
					"ccache sloppiness must not contain pch_defines when building Cemu")
			endif()

			foreach(language IN ITEMS C CXX OBJC OBJCXX)
				set(CMAKE_${language}_COMPILER_LAUNCHER "${CEMU_CCACHE_EXECUTABLE}")
			endforeach()
			message(STATUS "Using ccache: ${CEMU_CCACHE_EXECUTABLE}")
		else()
			message(WARNING
				"Ignoring unusable ccache executable: ${CEMU_CCACHE_EXECUTABLE}")
		endif()
	else()
		message(STATUS "ccache not found; continuing without a compiler launcher")
	endif()
endif()

option(CEMU_USE_UNITY_BUILD "Enable Unity Build across the complete build graph" ON)
set(CMAKE_UNITY_BUILD ${CEMU_USE_UNITY_BUILD})

# foundation still uses this legacy Azahar-facing switch. Keep one host-level
# source of truth so Cemu and foundation cannot silently select different modes.
set(CITRA_USE_UNITY_BUILD ${CEMU_USE_UNITY_BUILD}
	CACHE BOOL "Enable Unity Build for foundation targets" FORCE)
mark_as_advanced(CITRA_USE_UNITY_BUILD)

if (MSVC)
	set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT CemuBin)
	# floating point model: precise, fiber safe optimizations
	add_compile_options(/EHsc /fp:precise)
	if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
		# Speeds up static linking (especially helpful in incremental compilation)
		if((CMAKE_LINKER MATCHES ".*lld-link.*") AND (CMAKE_AR MATCHES ".*llvm-lib.*"))
			set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY STATIC_LIBRARY_OPTIONS /llvmlibthin)
		endif()
  	else()
	  add_compile_options(/GT)
	endif()
	# enable additional optimization flags for release builds
	add_compile_options($<$<CONFIG:Release,RelWithDebInfo>:/Oi>) # enable intrinsic functions
	add_compile_options($<$<CONFIG:Release,RelWithDebInfo>:/Ot>) # favor speed
endif()

if(CMAKE_GENERATOR STREQUAL "Xcode")
	set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY XCODE_STARTUP_PROJECT CemuBin)
endif()

if (APPLE)
    enable_language(OBJC OBJCXX)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.4")
endif()

if (UNIX AND NOT APPLE)
	option(ENABLE_WAYLAND "Build with Wayland support" ON)
	option(ENABLE_FERAL_GAMEMODE "Enables Feral Interactive GameMode Support" ON)
	option(ENABLE_BLUEZ "Build with Bluez support" ON)
endif()

if (APPLE)
    set(ENABLE_METAL_DEFAULT ON)
	set(ENABLE_OPENGL_DEFAULT OFF)
else()
    set(ENABLE_METAL_DEFAULT OFF)
	set(ENABLE_OPENGL_DEFAULT ON)
endif()

option(ENABLE_OPENGL "Enables the OpenGL backend" ${ENABLE_OPENGL_DEFAULT})
option(ENABLE_VULKAN "Enables the Vulkan backend" ON)
option(ENABLE_METAL "Enables the Metal backend" ${ENABLE_METAL_DEFAULT})
option(ENABLE_DISCORD_RPC "Enables the Discord Rich Presence feature" ON)

if (ENABLE_METAL AND NOT APPLE)
    message(FATAL_ERROR "Metal backend is only supported on Apple platforms")
endif()

# input backends
if (WIN32)
	option(ENABLE_XINPUT "Enables the usage of XInput" ON)
	option(ENABLE_DIRECTINPUT "Enables the usage of DirectInput" ON)
	add_compile_definitions(HAS_DIRECTINPUT)
endif()

option(ENABLE_HIDAPI "Build with HIDAPI" ON)
option(ENABLE_SDL "Enables the SDLController backend" ON)
option(ENABLE_LIBUSB "Build with libusb support" ON)

if (ENABLE_LIBUSB)
	add_compile_definitions(HAS_LIBUSB)
endif()

# audio backends
if (WIN32)
	option(ENABLE_DIRECTAUDIO "Enables the directaudio backend" ON)
	option(ENABLE_XAUDIO "Enables the xaudio backend" ON)
endif()
option(ENABLE_CUBEB "Enabled cubeb backend" ON)

option(ENABLE_WXWIDGETS "Build with wxWidgets UI (Currently required)" ON)

find_package(Threads REQUIRED)
include(CemuBundledDependencies)

if(ENABLE_SDL)
	add_compile_definitions(HAS_SDL)
endif()

# glslang versions older than 11.11.0 define targets without a namespace
if (NOT TARGET glslang::SPIRV AND TARGET SPIRV)
	add_library(glslang::SPIRV ALIAS SPIRV)
endif()

if (UNIX AND NOT APPLE AND NOT ANDROID)
	find_package(X11 REQUIRED)
	if (ENABLE_WAYLAND)
		find_package(Wayland REQUIRED Client)
		find_package(WaylandScanner REQUIRED)
		find_package(WaylandProtocols 1.15 REQUIRED)

		ecm_add_wayland_client_protocol(WAYLAND_PROTOCOL_SRCS
			PROTOCOL "${WaylandProtocols_DATADIR}/stable/viewporter/viewporter.xml"
			BASENAME viewporter)
		add_library(CemuWaylandProtocols STATIC ${WAYLAND_PROTOCOL_SRCS})
		target_include_directories(CemuWaylandProtocols PUBLIC "${CMAKE_CURRENT_BINARY_DIR}")
		target_include_directories(CemuWaylandProtocols PRIVATE ${Wayland_INCLUDE_DIRS})
		add_compile_definitions(HAS_WAYLAND)
	endif()
	find_package(GTK3 REQUIRED)

	if(ENABLE_BLUEZ)
		find_package(bluez REQUIRED)
		set(SUPPORTS_WIIMOTE ON)
		add_compile_definitions(HAS_BLUEZ)
	endif()

endif()

if (ENABLE_VULKAN)
	include_directories("dependencies/Vulkan-Headers/include")
	add_compile_definitions(ENABLE_VULKAN)
	add_compile_definitions(VK_NO_PROTOTYPES)
endif()

if (ENABLE_OPENGL)
	find_package(OpenGL REQUIRED)
	add_compile_definitions(ENABLE_OPENGL)
endif()

if (ENABLE_METAL)
    include_directories(${CMAKE_SOURCE_DIR}/dependencies/metal-cpp)

    add_compile_definitions(ENABLE_METAL)
endif()

if (ENABLE_DISCORD_RPC)
	add_compile_definitions(ENABLE_DISCORD_RPC)
endif()

if (NOT EXISTS "${CMAKE_SOURCE_DIR}/dependencies/foundation/CMakeLists.txt")
	message(FATAL_ERROR
		"foundation submodule missing, run: git submodule update --init --recursive")
endif()
add_subdirectory("dependencies/foundation" EXCLUDE_FROM_ALL)
# foundation_allocator stores reflection objects in public allocator types,
# while foundation_meta_reflection also uses the allocator. Declare the
# complete static-library cycle so CMake repeats the archives at final link.
target_link_libraries(foundation_allocator PUBLIC
	spatial::foundation_meta_reflection
)

if (ENABLE_HIDAPI)
	set(SUPPORTS_WIIMOTE ON)
	add_compile_definitions(HAS_HIDAPI)
endif ()

if(UNIX AND NOT APPLE AND NOT ANDROID)
	if(ENABLE_FERAL_GAMEMODE)
		add_compile_definitions(ENABLE_FERAL_GAMEMODE)
		add_subdirectory(dependencies/gamemode EXCLUDE_FROM_ALL)
		target_include_directories(gamemode INTERFACE ./dependencies/gamemode/lib)
	endif()
endif()

if (ENABLE_CUBEB)
	option(BUILD_TESTS "" OFF)
	option(BUILD_TOOLS "" OFF)
	option(BUNDLE_SPEEX "" OFF)
	set(USE_WINMM OFF CACHE BOOL "")
	add_subdirectory("dependencies/cubeb" EXCLUDE_FROM_ALL SYSTEM)
	set_property(TARGET cubeb PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
	add_library(cubeb::cubeb ALIAS cubeb)
	add_compile_definitions("HAS_CUBEB=1")
endif()

add_subdirectory("dependencies/ih264d" EXCLUDE_FROM_ALL)

if (CMAKE_OSX_ARCHITECTURES)
	set(CEMU_ARCHITECTURE ${CMAKE_OSX_ARCHITECTURES})
else()
	set(CEMU_ARCHITECTURE ${CMAKE_SYSTEM_PROCESSOR})
endif()
if(CEMU_ARCHITECTURE MATCHES "(aarch64)|(AARCH64)|(arm64)|(ARM64)")
	add_subdirectory("dependencies/xbyak_aarch64" EXCLUDE_FROM_ALL)
endif()

find_package(ZArchive QUIET)
if (NOT ZArchive_FOUND)
	add_subdirectory("dependencies/ZArchive" EXCLUDE_FROM_ALL)
endif()

if(ANDROID)
	if(CMAKE_SYSTEM_PROCESSOR MATCHES "(aarch64)|(AARCH64)")
		add_subdirectory("dependencies/libadrenotools" EXCLUDE_FROM_ALL)
	endif()
	add_subdirectory("dependencies/stb" EXCLUDE_FROM_ALL)
endif()

add_subdirectory(src)

if (CEMU_USE_UNITY_BUILD)
	# These targets rely heavily on translation-unit isolation. Keep them on
	# their supported build path without modifying bundled dependency sources.
	foreach(target IN ITEMS
		CemuWxGui
		wxadv
		wxaui
		wxbase
		wxcore
		wxgl
		wxhtml
		wxlexilla
		wxmedia
		wxnet
		wxpropgrid
		wxqa
		wxrc
		wxribbon
		wxrichtext
		wxscintilla
		wxstc
		wxwebview
		wxxml
		wxxrc
		bs_obj
		cemu_boost_iostreams
		cemu_boost_program_options
		cemu_libusb
		crypto_obj
		cubeb
		foundation_third_party_libevent
		glslang
		ih264d
		imguiImpl
		libcurl_object
		libcurl_static
		libzstd_static
		libwebpmux
		sharpyuv
		ssl_obj
		tls_obj
		webp
		webpdecode
		webpdecoder
		webpdemux
		webpdsp
		webpdspdecode
		webpencode
		webputils
		webputilsdecode
		wxexpat
		wxjpeg
		wxpng
		wxregex
		tiff
		wxtiff
		wxzlib
		zip
		zlib
	)
		if (TARGET ${target})
			set_target_properties(${target} PROPERTIES UNITY_BUILD OFF)
		endif()
	endforeach()
endif()

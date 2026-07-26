include_guard(GLOBAL)

function(cemu_add_bundled_subdirectory dependency source_directory)
	add_subdirectory(
		"${CMAKE_SOURCE_DIR}/dependencies/${source_directory}"
		"${CMAKE_BINARY_DIR}/dependencies/${dependency}"
		EXCLUDE_FROM_ALL
		SYSTEM
	)
endfunction()

if (CEMU_USE_BUNDLED_DEPENDENCIES)
	set(FMT_DOC OFF CACHE BOOL "" FORCE)
	set(FMT_INSTALL OFF CACHE BOOL "" FORCE)
	set(FMT_TEST OFF CACHE BOOL "" FORCE)
	cemu_add_bundled_subdirectory(fmt fmt)

	set(ZSTD_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
	set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
	set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
	set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
	set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
	set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
	cemu_add_bundled_subdirectory(zstd zstd/build/cmake)
	if (NOT TARGET zstd::zstd)
		add_library(zstd::zstd ALIAS libzstd_static)
	endif()

	set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
	set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
	set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
	set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
	set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
	set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
	set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
	set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
	cemu_add_bundled_subdirectory(glslang glslang)
	set(glslang_VERSION "15.1.0")
	# Keep Cemu's installed-package include spelling available for the in-tree
	# glslang layout: <glslang/SPIRV/GlslangToSpv.h>.
	target_include_directories(glslang SYSTEM INTERFACE
		"${CMAKE_SOURCE_DIR}/dependencies"
	)

	if (CEMU_ENABLE_FOUNDATION)
		set(CRYPTOPP_BUILD_DOCUMENTATION OFF CACHE BOOL "" FORCE)
		set(CRYPTOPP_BUILD_TESTING OFF CACHE BOOL "" FORCE)
		set(CRYPTOPP_INSTALL OFF CACHE BOOL "" FORCE)
		set(CRYPTOPP_SOURCES "${CMAKE_SOURCE_DIR}/dependencies/cryptopp" CACHE PATH "" FORCE)
		cemu_add_bundled_subdirectory(cryptopp cryptopp-cmake)
	endif()

	if (ENABLE_VULKAN AND NOT TARGET vulkan-headers)
		add_library(vulkan-headers INTERFACE)
		target_include_directories(vulkan-headers INTERFACE
			"${CMAKE_SOURCE_DIR}/dependencies/Vulkan-Headers/include"
		)
	endif()

	if (ENABLE_LIBUSB)
		set(CEMU_LIBUSB_SOURCE_DIR "${CMAKE_SOURCE_DIR}/dependencies/libusb")
		set(CEMU_LIBUSB_BINARY_DIR "${CMAKE_BINARY_DIR}/dependencies/libusb")
		set(CEMU_LIBUSB_INCLUDE_DIR "${CEMU_LIBUSB_BINARY_DIR}/include")

		file(MAKE_DIRECTORY "${CEMU_LIBUSB_INCLUDE_DIR}/libusb-1.0")
		configure_file(
			"${CEMU_LIBUSB_SOURCE_DIR}/libusb/libusb.h"
			"${CEMU_LIBUSB_INCLUDE_DIR}/libusb-1.0/libusb.h"
			COPYONLY
		)

		add_library(cemu_libusb STATIC EXCLUDE_FROM_ALL
			"${CEMU_LIBUSB_SOURCE_DIR}/libusb/core.c"
			"${CEMU_LIBUSB_SOURCE_DIR}/libusb/descriptor.c"
			"${CEMU_LIBUSB_SOURCE_DIR}/libusb/hotplug.c"
			"${CEMU_LIBUSB_SOURCE_DIR}/libusb/io.c"
			"${CEMU_LIBUSB_SOURCE_DIR}/libusb/strerror.c"
			"${CEMU_LIBUSB_SOURCE_DIR}/libusb/sync.c"
		)
		add_library(libusb::libusb ALIAS cemu_libusb)

		target_include_directories(cemu_libusb
			BEFORE
			PUBLIC
				"${CEMU_LIBUSB_INCLUDE_DIR}"
			PRIVATE
				"${CEMU_LIBUSB_SOURCE_DIR}/libusb"
				"${CEMU_LIBUSB_BINARY_DIR}"
		)

		if (WIN32 OR CYGWIN)
			target_sources(cemu_libusb PRIVATE
				"${CEMU_LIBUSB_SOURCE_DIR}/libusb/os/events_windows.c"
				"${CEMU_LIBUSB_SOURCE_DIR}/libusb/os/threads_windows.c"
				"${CEMU_LIBUSB_SOURCE_DIR}/libusb/os/windows_common.c"
				"${CEMU_LIBUSB_SOURCE_DIR}/libusb/os/windows_usbdk.c"
				"${CEMU_LIBUSB_SOURCE_DIR}/libusb/os/windows_winusb.c"
			)
			if (MSVC)
				target_compile_options(cemu_libusb PRIVATE /utf-8)
				target_include_directories(cemu_libusb BEFORE PRIVATE
					"${CEMU_LIBUSB_SOURCE_DIR}/msvc"
				)
			endif()
			set(OS_WINDOWS TRUE)
			set(PLATFORM_WINDOWS TRUE)
		elseif (APPLE)
			target_sources(cemu_libusb PRIVATE
				"${CEMU_LIBUSB_SOURCE_DIR}/libusb/os/darwin_usb.c"
				"${CEMU_LIBUSB_SOURCE_DIR}/libusb/os/events_posix.c"
				"${CEMU_LIBUSB_SOURCE_DIR}/libusb/os/threads_posix.c"
			)
			find_library(COREFOUNDATION_LIBRARY CoreFoundation REQUIRED)
			find_library(IOKIT_LIBRARY IOKit REQUIRED)
			find_library(SECURITY_LIBRARY Security REQUIRED)
			find_library(OBJC_LIBRARY objc REQUIRED)
			target_link_libraries(cemu_libusb PRIVATE
				${COREFOUNDATION_LIBRARY}
				${IOKIT_LIBRARY}
				${SECURITY_LIBRARY}
				${OBJC_LIBRARY}
				Threads::Threads
			)
			set(OS_DARWIN TRUE)
			set(PLATFORM_POSIX TRUE)
		elseif (ANDROID)
			target_sources(cemu_libusb PRIVATE
				"${CEMU_LIBUSB_SOURCE_DIR}/libusb/os/events_posix.c"
				"${CEMU_LIBUSB_SOURCE_DIR}/libusb/os/linux_netlink.c"
				"${CEMU_LIBUSB_SOURCE_DIR}/libusb/os/linux_usbfs.c"
				"${CEMU_LIBUSB_SOURCE_DIR}/libusb/os/threads_posix.c"
			)
			target_link_libraries(cemu_libusb PRIVATE log Threads::Threads)
			set(OS_LINUX TRUE)
			set(PLATFORM_POSIX TRUE)
		elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
			target_sources(cemu_libusb PRIVATE
				"${CEMU_LIBUSB_SOURCE_DIR}/libusb/os/events_posix.c"
				"${CEMU_LIBUSB_SOURCE_DIR}/libusb/os/linux_netlink.c"
				"${CEMU_LIBUSB_SOURCE_DIR}/libusb/os/linux_usbfs.c"
				"${CEMU_LIBUSB_SOURCE_DIR}/libusb/os/threads_posix.c"
			)
			target_link_libraries(cemu_libusb PRIVATE Threads::Threads)
			set(OS_LINUX TRUE)
			set(PLATFORM_POSIX TRUE)
		elseif (CMAKE_SYSTEM_NAME STREQUAL "NetBSD")
			target_sources(cemu_libusb PRIVATE
				"${CEMU_LIBUSB_SOURCE_DIR}/libusb/os/events_posix.c"
				"${CEMU_LIBUSB_SOURCE_DIR}/libusb/os/netbsd_usb.c"
				"${CEMU_LIBUSB_SOURCE_DIR}/libusb/os/threads_posix.c"
			)
			target_link_libraries(cemu_libusb PRIVATE Threads::Threads)
			set(OS_NETBSD TRUE)
			set(PLATFORM_POSIX TRUE)
		elseif (CMAKE_SYSTEM_NAME STREQUAL "OpenBSD")
			target_sources(cemu_libusb PRIVATE
				"${CEMU_LIBUSB_SOURCE_DIR}/libusb/os/events_posix.c"
				"${CEMU_LIBUSB_SOURCE_DIR}/libusb/os/openbsd_usb.c"
				"${CEMU_LIBUSB_SOURCE_DIR}/libusb/os/threads_posix.c"
			)
			target_link_libraries(cemu_libusb PRIVATE Threads::Threads)
			set(OS_OPENBSD TRUE)
			set(PLATFORM_POSIX TRUE)
		endif()

		include(CheckFunctionExists)
		include(CheckIncludeFiles)
		include(CheckTypeSize)
		check_include_files(asm/types.h HAVE_ASM_TYPES_H)
		check_function_exists(gettimeofday HAVE_GETTIMEOFDAY)
		check_include_files(linux/filter.h HAVE_LINUX_FILTER_H)
		check_include_files(linux/netlink.h HAVE_LINUX_NETLINK_H)
		check_function_exists(eventfd HAVE_EVENTFD)
		check_function_exists(timerfd_create HAVE_TIMERFD)
		check_include_files(signal.h HAVE_SIGNAL_H)
		check_include_files(strings.h HAVE_STRINGS_H)
		check_type_size("struct timespec" STRUCT_TIMESPEC)
		check_function_exists(syslog HAVE_SYSLOG_FUNC)
		check_include_files(syslog.h HAVE_SYSLOG_H)
		check_include_files(sys/socket.h HAVE_SYS_SOCKET_H)
		check_include_files(sys/time.h HAVE_SYS_TIME_H)
		check_include_files(sys/types.h HAVE_SYS_TYPES_H)
		check_function_exists(clock_gettime HAVE_CLOCK_GETTIME)

		file(MAKE_DIRECTORY "${CEMU_LIBUSB_BINARY_DIR}")
		configure_file(
			"${CMAKE_SOURCE_DIR}/cmake/libusb_config.h.in"
			"${CEMU_LIBUSB_BINARY_DIR}/config.h"
		)

		set_target_properties(cemu_libusb PROPERTIES
			FOLDER "Dependencies"
			POSITION_INDEPENDENT_CODE ON
		)
	endif()
endif()

add_library(cemu_rapidjson INTERFACE)
if (CEMU_USE_BUNDLED_DEPENDENCIES AND CEMU_ENABLE_FOUNDATION)
	target_link_libraries(cemu_rapidjson INTERFACE spatial::third_party_rapidjson)
else()
	find_package(RapidJSON REQUIRED)
	target_include_directories(cemu_rapidjson INTERFACE ${RAPIDJSON_INCLUDE_DIRS})
endif()
add_library(Cemu::rapidjson ALIAS cemu_rapidjson)

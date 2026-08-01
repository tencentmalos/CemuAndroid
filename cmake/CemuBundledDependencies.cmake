include_guard(GLOBAL)

function(cemu_add_bundled_subdirectory dependency source_directory)
	add_subdirectory(
		"${CMAKE_SOURCE_DIR}/dependencies/${source_directory}"
		"${CMAKE_BINARY_DIR}/dependencies/${dependency}"
		EXCLUDE_FROM_ALL
		SYSTEM
	)
endfunction()

add_library(cemu_async_simple_headers INTERFACE)
target_include_directories(cemu_async_simple_headers INTERFACE
	"${CMAKE_SOURCE_DIR}/dependencies/foundation/third_party/yalantinglibs/include/ylt/thirdparty"
)
add_library(Cemu::async_simple_headers ALIAS cemu_async_simple_headers)

# Boost is intentionally consumed as a repository-managed source subset.
# Keep the conventional imported target names so Cemu code does not need to
# know whether Boost came from a package manager or this submodule.
add_library(cemu_boost_headers INTERFACE)
target_include_directories(cemu_boost_headers SYSTEM INTERFACE
	"${CMAKE_SOURCE_DIR}/dependencies/boost"
)
add_library(Boost::boost ALIAS cemu_boost_headers)
add_library(Boost::headers ALIAS cemu_boost_headers)
add_library(Boost::nowide ALIAS cemu_boost_headers)

	file(GLOB CEMU_BOOST_PROGRAM_OPTIONS_SOURCES CONFIGURE_DEPENDS
		"${CMAKE_SOURCE_DIR}/dependencies/boost/libs/program_options/src/*.cpp"
	)
	add_library(cemu_boost_program_options STATIC EXCLUDE_FROM_ALL
		${CEMU_BOOST_PROGRAM_OPTIONS_SOURCES}
	)
	target_link_libraries(cemu_boost_program_options PUBLIC cemu_boost_headers)
	target_compile_definitions(cemu_boost_program_options PUBLIC BOOST_ALL_NO_LIB)
	set_target_properties(cemu_boost_program_options PROPERTIES
		POSITION_INDEPENDENT_CODE ON
	)
	add_library(Boost::program_options ALIAS cemu_boost_program_options)

	if (ANDROID)
		add_library(cemu_boost_iostreams STATIC EXCLUDE_FROM_ALL
			"${CMAKE_SOURCE_DIR}/dependencies/boost/libs/iostreams/src/file_descriptor.cpp"
			"${CMAKE_SOURCE_DIR}/dependencies/boost/libs/iostreams/src/mapped_file.cpp"
		)
		target_link_libraries(cemu_boost_iostreams PUBLIC cemu_boost_headers)
		target_compile_definitions(cemu_boost_iostreams PUBLIC BOOST_ALL_NO_LIB)
		set_target_properties(cemu_boost_iostreams PROPERTIES
			POSITION_INDEPENDENT_CODE ON
		)
		add_library(Boost::iostreams ALIAS cemu_boost_iostreams)
	endif()

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

	set(ZLIB_COMPAT ON CACHE BOOL "" FORCE)
	set(ZLIB_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
	set(ZLIBNG_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
	set(WITH_GTEST OFF CACHE BOOL "" FORCE)
	set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)
	set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
	cemu_add_bundled_subdirectory(zlib-ng zlib-ng)
	if (NOT TARGET ZLIB::ZLIB)
		add_library(ZLIB::ZLIB ALIAS zlib)
	endif()
	set(ZLIB_FOUND TRUE)
	set(ZLIB_VERSION_STRING "2.2.5")
	set(ZLIB_INCLUDE_DIRS
		"${CMAKE_BINARY_DIR}/dependencies/zlib-ng"
		"${CMAKE_SOURCE_DIR}/dependencies/zlib-ng"
	)
	set(ZLIB_LIBRARIES ZLIB::ZLIB)

	set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
	cemu_add_bundled_subdirectory(libressl libressl)
	target_include_directories(crypto PUBLIC
		"${CMAKE_SOURCE_DIR}/dependencies/libressl/include"
		"${CMAKE_BINARY_DIR}/include"
	)
	target_include_directories(ssl PUBLIC
		"${CMAKE_SOURCE_DIR}/dependencies/libressl/include"
		"${CMAKE_BINARY_DIR}/include"
	)
	add_library(OpenSSL::Crypto ALIAS crypto)
	add_library(OpenSSL::SSL ALIAS ssl)
	set(OpenSSL_FOUND TRUE)
	set(OPENSSL_FOUND TRUE)
	set(OPENSSL_VERSION "3.8.0")
	set(OPENSSL_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/dependencies/libressl/include")
	set(OPENSSL_INCLUDE_DIRS "${OPENSSL_INCLUDE_DIR}")
	set(OPENSSL_LIBRARIES OpenSSL::SSL OpenSSL::Crypto)

	set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
	set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
	set(BUILD_LIBCURL_DOCS OFF CACHE BOOL "" FORCE)
	set(BUILD_MISC_DOCS OFF CACHE BOOL "" FORCE)
	set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
	set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
	set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
	set(CURL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
	set(CURL_DISABLE_LDAP ON CACHE BOOL "" FORCE)
	set(CURL_DISABLE_LDAPS ON CACHE BOOL "" FORCE)
	set(CURL_ENABLE_EXPORT_TARGET OFF CACHE BOOL "" FORCE)
	set(CURL_USE_OPENSSL ON CACHE BOOL "" FORCE)
	set(CURL_USE_PKGCONFIG OFF CACHE BOOL "" FORCE)
	set(USE_NGHTTP2 OFF CACHE BOOL "" FORCE)
	set(CURL_ZLIB ON CACHE STRING "" FORCE)
	set(CURL_BROTLI OFF CACHE STRING "" FORCE)
	# Cemu links its pinned zstd target directly. Avoid making curl/libzip run a
	# second package-discovery path for that same implementation.
	set(CURL_ZSTD OFF CACHE STRING "" FORCE)
	set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
	set(CURL_USE_LIBSSH2 OFF CACHE BOOL "" FORCE)
	set(CURL_USE_LIBSSH OFF CACHE BOOL "" FORCE)
	set(CURL_USE_GSASL OFF CACHE BOOL "" FORCE)
	set(CURL_USE_GSSAPI OFF CACHE BOOL "" FORCE)
	set(USE_LIBIDN2 OFF CACHE BOOL "" FORCE)
	# curl's configure checks run in isolated try_compile projects where aliases
	# to parent-project targets are not visible. These values describe the pinned
	# LibreSSL headers and avoid probing a different system OpenSSL installation.
	set(HAVE_AWSLC FALSE CACHE INTERNAL "" FORCE)
	set(HAVE_BORINGSSL FALSE CACHE INTERNAL "" FORCE)
	set(HAVE_LIBRESSL TRUE CACHE INTERNAL "" FORCE)
	set(HAVE_OPENSSL_SRP FALSE CACHE INTERNAL "" FORCE)
	set(HAVE_SSL_SET0_WBIO FALSE CACHE INTERNAL "" FORCE)
	set(CURL_DISABLE_SRP ON CACHE BOOL "" FORCE)
	cemu_add_bundled_subdirectory(curl curl)

	set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
	set(PUGIXML_BUILD_TESTS OFF CACHE BOOL "" FORCE)
	set(PUGIXML_INSTALL OFF CACHE BOOL "" FORCE)
	cemu_add_bundled_subdirectory(pugixml pugixml)

	set(GLM_BUILD_INSTALL OFF CACHE BOOL "" FORCE)
	set(GLM_BUILD_LIBRARY OFF CACHE BOOL "" FORCE)
	set(GLM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
	cemu_add_bundled_subdirectory(glm glm)

	if (ENABLE_SDL)
		set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
		set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
		set(SDL_HIDAPI_LIBUSB OFF CACHE BOOL "" FORCE)
		set(SDL_SHARED OFF CACHE BOOL "" FORCE)
		set(SDL_STATIC ON CACHE BOOL "" FORCE)
		set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
		set(SDL_TESTS OFF CACHE BOOL "" FORCE)
		set(SDL_UNINSTALL OFF CACHE BOOL "" FORCE)
		cemu_add_bundled_subdirectory(SDL SDL)
	endif()

	if (ENABLE_WXWIDGETS)
		set(BUILD_DOC OFF CACHE BOOL "" FORCE)
		set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
		set(BUILD_OSSFUZZ OFF CACHE BOOL "" FORCE)
		set(BUILD_REGRESS OFF CACHE BOOL "" FORCE)
		set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
		set(BUILD_TOOLS OFF CACHE BOOL "" FORCE)
		set(ENABLE_BZIP2 OFF CACHE BOOL "" FORCE)
		set(ENABLE_COMMONCRYPTO OFF CACHE BOOL "" FORCE)
		set(ENABLE_GNUTLS OFF CACHE BOOL "" FORCE)
		set(ENABLE_LZMA OFF CACHE BOOL "" FORCE)
		set(ENABLE_MBEDTLS OFF CACHE BOOL "" FORCE)
		set(ENABLE_OPENSSL ON CACHE BOOL "" FORCE)
		set(ENABLE_WINDOWS_CRYPTO OFF CACHE BOOL "" FORCE)
		set(ENABLE_ZSTD OFF CACHE BOOL "" FORCE)
		set(LIBZIP_DO_INSTALL OFF CACHE BOOL "" FORCE)
		cemu_add_bundled_subdirectory(libzip libzip)

		set(wxBUILD_BENCHMARKS OFF CACHE STRING "" FORCE)
		set(wxBUILD_DEMOS OFF CACHE STRING "" FORCE)
		set(wxBUILD_INSTALL OFF CACHE BOOL "" FORCE)
		set(wxBUILD_LOCALES OFF CACHE STRING "" FORCE)
		set(wxBUILD_PRECOMP OFF CACHE STRING "" FORCE)
		set(wxBUILD_SAMPLES OFF CACHE STRING "" FORCE)
		set(wxBUILD_SHARED OFF CACHE BOOL "" FORCE)
		set(wxBUILD_TESTS OFF CACHE STRING "" FORCE)
		set(wxUSE_EXPAT builtin CACHE STRING "" FORCE)
		set(wxUSE_LIBJPEG builtin CACHE STRING "" FORCE)
		set(wxUSE_LIBPNG builtin CACHE STRING "" FORCE)
		set(wxUSE_LIBTIFF builtin CACHE STRING "" FORCE)
		set(wxUSE_LIBWEBP builtin CACHE STRING "" FORCE)
		set(wxUSE_ZLIB builtin CACHE STRING "" FORCE)
		cemu_add_bundled_subdirectory(wxWidgets wxWidgets)
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

	set(CRYPTOPP_BUILD_DOCUMENTATION OFF CACHE BOOL "" FORCE)
	set(CRYPTOPP_BUILD_TESTING OFF CACHE BOOL "" FORCE)
	set(CRYPTOPP_INSTALL OFF CACHE BOOL "" FORCE)
	set(CRYPTOPP_SOURCES "${CMAKE_SOURCE_DIR}/dependencies/cryptopp" CACHE PATH "" FORCE)
	cemu_add_bundled_subdirectory(cryptopp cryptopp-cmake)

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

add_library(cemu_rapidjson INTERFACE)
target_link_libraries(cemu_rapidjson INTERFACE spatial::third_party_rapidjson)
add_library(Cemu::rapidjson ALIAS cemu_rapidjson)

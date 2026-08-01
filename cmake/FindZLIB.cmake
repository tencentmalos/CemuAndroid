if (TARGET ZLIB::ZLIB)
	set(ZLIB_FOUND TRUE)
	set(ZLIB_VERSION "2.2.5")
	set(ZLIB_VERSION_STRING "${ZLIB_VERSION}")
	set(ZLIB_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/dependencies/zlib-ng")
	set(ZLIB_INCLUDE_DIRS
		"${CMAKE_BINARY_DIR}/dependencies/zlib-ng"
		"${ZLIB_INCLUDE_DIR}"
	)
	set(ZLIB_LIBRARY ZLIB::ZLIB)
	set(ZLIB_LIBRARIES ZLIB::ZLIB)

	include(FindPackageHandleStandardArgs)
	find_package_handle_standard_args(ZLIB
		REQUIRED_VARS ZLIB_LIBRARY ZLIB_INCLUDE_DIR
		VERSION_VAR ZLIB_VERSION
	)
	return()
endif()

include("${CMAKE_ROOT}/Modules/FindZLIB.cmake")

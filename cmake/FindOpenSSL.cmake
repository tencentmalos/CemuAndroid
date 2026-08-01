if (TARGET OpenSSL::Crypto AND TARGET OpenSSL::SSL)
	set(OpenSSL_FOUND TRUE)
	set(OPENSSL_FOUND TRUE)
	set(OPENSSL_VERSION "3.8.0")
	set(OPENSSL_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/dependencies/libressl/include")
	set(OPENSSL_INCLUDE_DIRS "${OPENSSL_INCLUDE_DIR}")
	set(OPENSSL_CRYPTO_LIBRARY OpenSSL::Crypto)
	set(OPENSSL_SSL_LIBRARY OpenSSL::SSL)
	set(OPENSSL_LIBRARIES OpenSSL::SSL OpenSSL::Crypto)

	include(FindPackageHandleStandardArgs)
	find_package_handle_standard_args(OpenSSL
		REQUIRED_VARS OPENSSL_CRYPTO_LIBRARY OPENSSL_SSL_LIBRARY OPENSSL_INCLUDE_DIR
		VERSION_VAR OPENSSL_VERSION
	)
	return()
endif()

include("${CMAKE_ROOT}/Modules/FindOpenSSL.cmake")

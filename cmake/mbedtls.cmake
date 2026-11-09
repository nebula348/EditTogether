# mbedTLS 3.6.2 (Apache-2.0), built from the official release tarball and linked statically.
# It provides TLS for the WebSocket/HTTPS client in src/net (Android has no public TLS library for native code).
include(FetchContent)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(GEN_FILES OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)
FetchContent_Declare(mbedtls
    URL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.2/mbedtls-3.6.2.tar.bz2
    URL_HASH SHA256=8b54fb9bcf4d5a7078028e0520acddefb7900b3e66fec7f7175bb5b7d85ccdca
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(_et_pic ${CMAKE_POSITION_INDEPENDENT_CODE})
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
FetchContent_MakeAvailable(mbedtls)
set(CMAKE_POSITION_INDEPENDENT_CODE ${_et_pic})
set(ET_MBEDTLS_LIBS mbedtls mbedx509 mbedcrypto)

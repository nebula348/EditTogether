cmake_minimum_required(VERSION 3.25)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

project(EditTogether VERSION 0.6.0)

if (NOT ANDROID OR NOT CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a")
    message(FATAL_ERROR "EditTogether targets Android arm64-v8a only. Build with: geode build -p android64")
endif()

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_library(${PROJECT_NAME} SHARED ${SOURCES})
target_include_directories(${PROJECT_NAME} PRIVATE src)

# TLS for the online (WebSocket/HTTPS) client, and zlib (in the NDK) for compressed relay chunks.
include(cmake/mbedtls.cmake)
target_link_libraries(${PROJECT_NAME} z ${ET_MBEDTLS_LIBS})

if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "GEODE_SDK environment variable is not set")
endif()

add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)
setup_geode_mod(${PROJECT_NAME})

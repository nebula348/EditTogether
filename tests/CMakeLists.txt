# Standalone test project (does not need Geode).
#   Desktop:  cmake -S tests -B build-tests && cmake --build build-tests
#   Android:  scripts/test-android.ps1
cmake_minimum_required(VERSION 3.25)
project(EditTogetherTests CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(../cmake/mbedtls.cmake)

set(CORE ../src/core/Protocol.cpp ../src/core/HostCore.cpp ../src/core/ClientCore.cpp ../src/core/PlayBuffer.cpp)
set(RELAY ../src/net/Json.cpp ../src/net/Tls.cpp ../src/net/WebSocket.cpp ../src/net/Http.cpp ../src/net/ApinatorClient.cpp
          ../src/net/RelayTransport.cpp ../src/net/ChunkLink.cpp)

add_executable(core_tests core_tests.cpp ${CORE})
target_include_directories(core_tests PRIVATE ../src)

if (NOT WIN32)
    add_executable(net_tests net_tests.cpp ../src/net/Socket.cpp ../src/net/Discovery.cpp ../src/net/ChunkLink.cpp ${CORE})
    target_link_libraries(net_tests z)
    target_include_directories(net_tests PRIVATE ../src)

    add_executable(relay_tests relay_tests.cpp ${RELAY} ${CORE})
    target_link_libraries(relay_tests z ${ET_MBEDTLS_LIBS})
    target_include_directories(relay_tests PRIVATE ../src)

    # Talks to the real Apinator service through a running relay auth service (see tests/README.md).
    add_executable(relay_live relay_live.cpp ${RELAY} ${CORE})
    target_link_libraries(relay_live z ${ET_MBEDTLS_LIBS})
    target_include_directories(relay_live PRIVATE ../src)

    add_executable(et_bot ../tools/et_bot.cpp ../src/net/Socket.cpp ../src/net/Discovery.cpp ${RELAY} ${CORE})
    target_link_libraries(et_bot z ${ET_MBEDTLS_LIBS})
    target_include_directories(et_bot PRIVATE ../src)
endif()

cmake_minimum_required(VERSION 3.16)

# Test executables mirroring test/Makefile outputs

set(TEST_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/test_server_simple.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_dual_protocol.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/simple_https_client.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/simple_wss_client.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/simple_ws_client.cpp
)

# Common include dirs: use binary headers copied by top-level, and repo include for convenience
include_directories(
    ${CMAKE_CURRENT_BINARY_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

function(add_myframe_test name src)
    add_executable(${name} ${src})
    target_link_libraries(${name} myframe OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
endfunction()

add_myframe_test(test_server_simple ${CMAKE_CURRENT_SOURCE_DIR}/test_server_simple.cpp)
add_myframe_test(test_dual_protocol ${CMAKE_CURRENT_SOURCE_DIR}/test_dual_protocol.cpp)
add_myframe_test(simple_https_client ${CMAKE_CURRENT_SOURCE_DIR}/simple_https_client.cpp)
add_myframe_test(simple_wss_client   ${CMAKE_CURRENT_SOURCE_DIR}/simple_wss_client.cpp)
add_myframe_test(simple_ws_client    ${CMAKE_CURRENT_SOURCE_DIR}/simple_ws_client.cpp)


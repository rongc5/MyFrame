# Examples for MyFrame Framework

# Include directories (use the same as parent project)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../include)
include_directories(${CMAKE_CURRENT_BINARY_DIR}/../include)

# Common libraries - link with myframe library which includes all dependencies
set(COMMON_LIBS
    myframe
    Threads::Threads
    OpenSSL::SSL 
    OpenSSL::Crypto
)

# HTTP Server Example
add_executable(http_server simple_http_server.cpp)
target_link_libraries(http_server ${COMMON_LIBS})

# HTTPS Server Example
add_executable(https_server simple_https_server.cpp)
target_link_libraries(https_server ${COMMON_LIBS})

# WebSocket Secure Server Example
add_executable(wss_server simple_wss_server.cpp)
target_link_libraries(wss_server ${COMMON_LIBS})


# HTTP Client Example
add_executable(http_client simple_http_client.cpp)
target_link_libraries(http_client ${COMMON_LIBS})

# HTTPS Client Example
add_executable(https_client simple_https_client.cpp)
target_link_libraries(https_client ${COMMON_LIBS})

# WebSocket Client Example
add_executable(ws_client simple_ws_client.cpp)
target_link_libraries(ws_client ${COMMON_LIBS})

# Multi-protocol Client (router + factories)
add_executable(multi_proto_client multi_proto_client.cpp)
target_link_libraries(multi_proto_client ${COMMON_LIBS})

# WebSocket Echo Client (WsSyncClient)
add_executable(ws_echo_client ws_echo_client.cpp)
target_link_libraries(ws_echo_client ${COMMON_LIBS})
# Set output directory
set_target_properties(
    http_server https_server wss_server
    http_client https_client ws_client multi_proto_client ws_echo_client http_out_client
    PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/examples
)

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


# HTTP/2 Client Example
add_executable(h2_client simple_h2_client.cpp)
target_link_libraries(h2_client ${COMMON_LIBS})

# HTTP Outbound Client Example
add_executable(http_out_client http_out_client.cpp)
target_link_libraries(http_out_client ${COMMON_LIBS})

# Minimal business HTTP client example
add_executable(biz_http_client biz_http_client.cpp)
target_link_libraries(biz_http_client ${COMMON_LIBS})

# Unified router client for http/https/ws/wss using event loop
add_executable(router_client router_client.cpp)
target_link_libraries(router_client ${COMMON_LIBS})

# Router business client with headers/body and WS handler
add_executable(router_biz_client router_biz_client.cpp)
target_link_libraries(router_biz_client ${COMMON_LIBS})

# ClientConnRouter Example
add_executable(client_conn_factory_example client_conn_factory_example.cpp)
target_link_libraries(client_conn_factory_example ${COMMON_LIBS})

# XPROTO Demo (custom protocol via MultiProtocolFactory + CustomProbe)
add_executable(xproto_server xproto_server.cpp)
target_link_libraries(xproto_server ${COMMON_LIBS})
add_executable(xproto_client xproto_client.cpp)
target_link_libraries(xproto_client ${COMMON_LIBS})
# Stock system examples (optional if sources exist)
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/stock_index_server.cpp" AND
    EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/stock_bridge_server.cpp" AND
    EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/stock_driver.cpp")
    add_executable(stock_index_server stock_index_server.cpp)
    target_link_libraries(stock_index_server ${COMMON_LIBS})
    add_executable(stock_bridge_server stock_bridge_server.cpp)
    target_link_libraries(stock_bridge_server ${COMMON_LIBS})
    add_executable(stock_driver stock_driver.cpp)
    target_link_libraries(stock_driver ${COMMON_LIBS})
    set(HAVE_STOCK_EXAMPLES TRUE)
else()
    set(HAVE_STOCK_EXAMPLES FALSE)
endif()

# Multi-protocol and multi-threading examples
add_executable(demo_multi_protocol_server multi_protocol_server.cpp)
target_link_libraries(demo_multi_protocol_server ${COMMON_LIBS})
add_executable(demo_multi_thread_server multi_thread_server.cpp)
target_link_libraries(demo_multi_thread_server ${COMMON_LIBS})
# HTTP/2 Server Example (TLS + ALPN h2)
add_executable(h2_server simple_h2_server.cpp)
target_link_libraries(h2_server ${COMMON_LIBS})

# WS benchmark client
add_executable(ws_bench_client ws_bench_client.cpp)
target_link_libraries(ws_bench_client ${COMMON_LIBS})
# Set output directory
# Always-present example targets
set_target_properties(
    http_server https_server wss_server
    h2_client http_out_client biz_http_client router_client router_biz_client client_conn_factory_example xproto_server xproto_client ws_bench_client
    demo_multi_protocol_server demo_multi_thread_server h2_server
    PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/examples
)

# Optional stock targets
if (HAVE_STOCK_EXAMPLES)
    set_target_properties(
        stock_index_server stock_bridge_server stock_driver
        PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/examples
    )
endif()

# Multi-thread server fixed
# Optional: keep fixed multi-thread server if desired
# add_executable(multi_thread_server_fixed multi_thread_server_fixed.cpp)
# target_link_libraries(multi_thread_server_fixed myframe ${OpenSSL_LIBRARIES} ${CMAKE_THREAD_LIBS_INIT})
# if(TARGET build_encode)
#     add_dependencies(multi_thread_server_fixed build_encode)
# endif()

# Examples for MyFrame Framework

# Include directories (use the same as parent project)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../include)
include_directories(${CMAKE_CURRENT_BINARY_DIR}/../include)

# Common libraries - link with myframe library which includes all dependencies
# CMake 2.8 compatible
set(COMMON_LIBS
    myframe
    ${CMAKE_THREAD_LIBS_INIT}
    ${OPENSSL_LIBRARIES}
)

# HTTP Server Example
add_executable(http_server simple_http_server.cpp)
target_link_libraries(http_server ${COMMON_LIBS})

# Level demonstrations
add_executable(level1_multi_protocol level1_multi_protocol.cpp)
target_link_libraries(level1_multi_protocol ${COMMON_LIBS})

add_executable(level2_multi_protocol level2_multi_protocol.cpp)
target_link_libraries(level2_multi_protocol ${COMMON_LIBS})

add_executable(level3_custom_echo level3_custom_echo.cpp)
target_link_libraries(level3_custom_echo ${COMMON_LIBS})

add_executable(level3_multi_protocol level3_multi_protocol.cpp)
target_link_libraries(level3_multi_protocol ${COMMON_LIBS})

# Thread user-data API demo
add_executable(thread_user_data_demo thread_user_data_demo.cpp)
target_link_libraries(thread_user_data_demo ${COMMON_LIBS})

# Async HTTP Server Demo - showcases async response handling
add_executable(async_http_demo async_http_server_demo.cpp)
target_link_libraries(async_http_demo ${COMMON_LIBS})

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

# TLS multi-fetch demo (HTTPS client sample)
add_executable(tls_multi_fetch tls_multi_fetch.cpp)
target_link_libraries(tls_multi_fetch ${COMMON_LIBS})

# HTTP/2 Outbound Client using out_connect
add_executable(http2_out_client http2_out_client.cpp)
target_link_libraries(http2_out_client ${COMMON_LIBS})

# Minimal business HTTP client example
add_executable(biz_http_client biz_http_client.cpp)
target_link_libraries(biz_http_client ${COMMON_LIBS})

# HTTP graceful close demo
add_executable(http_close_demo http_close_demo.cpp)
target_link_libraries(http_close_demo ${COMMON_LIBS})

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

# HTTP Server Close Demo (graceful close after response)
add_executable(http_server_close_demo http_server_close_demo.cpp)
target_link_libraries(http_server_close_demo ${COMMON_LIBS})

# WS benchmark client
add_executable(ws_bench_client ws_bench_client.cpp)
target_link_libraries(ws_bench_client ${COMMON_LIBS})

# WebSocket broadcast demos
add_executable(ws_broadcast_user ws_broadcast_user.cpp)
target_link_libraries(ws_broadcast_user ${COMMON_LIBS})
add_executable(ws_broadcast_periodic ws_broadcast_periodic.cpp)
target_link_libraries(ws_broadcast_periodic ${COMMON_LIBS})

# Framework WS smoke-test pair
add_executable(framework_ws_server framework_ws_server.cpp)
target_link_libraries(framework_ws_server ${COMMON_LIBS})
add_executable(framework_ws_client framework_ws_client.cpp)
target_link_libraries(framework_ws_client ${COMMON_LIBS})
add_executable(framework_ws_client_idle framework_ws_client_idle.cpp)
target_link_libraries(framework_ws_client_idle ${COMMON_LIBS})

# StickBridge client demo leveraging myframe websocket stack
add_executable(ws_stickbridge_client ws_stickbridge_client.cpp)
target_link_libraries(ws_stickbridge_client ${COMMON_LIBS})

# Unified Protocol Architecture - Simple HTTP Example (Level 1 API)
add_executable(unified_simple_http unified_simple_http.cpp)
target_link_libraries(unified_simple_http ${COMMON_LIBS})

# Unified Protocol Architecture - Mixed HTTP + WebSocket Example (Level 1 API)
add_executable(unified_mixed_server unified_mixed_server.cpp)
target_link_libraries(unified_mixed_server ${COMMON_LIBS})

# Unified Protocol Architecture - WebSocket Client Test
add_executable(unified_ws_client_test unified_ws_client_test.cpp)
target_link_libraries(unified_ws_client_test ${COMMON_LIBS})

# Unified Protocol Architecture - Level 2 Context Demo
add_executable(unified_level2_demo unified_level2_demo.cpp)
target_link_libraries(unified_level2_demo ${COMMON_LIBS})

# Unified Protocol Architecture - HTTPS/WSS Server Demo
add_executable(unified_https_wss_server unified_https_wss_server.cpp)
target_link_libraries(unified_https_wss_server ${COMMON_LIBS})

# Unified Protocol Architecture - Async Response Demo
add_executable(unified_async_demo unified_async_demo.cpp)
target_link_libraries(unified_async_demo ${COMMON_LIBS})

# Simple Async Test with Timer
add_executable(simple_async_test simple_async_test.cpp)
target_link_libraries(simple_async_test ${COMMON_LIBS})

# Test memory management
add_executable(test_memory_management test_memory_management.cpp)
target_link_libraries(test_memory_management ${COMMON_LIBS})

# Test edge cases
add_executable(test_edge_cases test_edge_cases.cpp)
target_link_libraries(test_edge_cases ${COMMON_LIBS})

# Set output directory
# Always-present example targets
set_target_properties(
    http_server async_http_demo https_server wss_server
    level1_multi_protocol level2_multi_protocol level3_custom_echo level3_multi_protocol
    thread_user_data_demo test_memory_management test_edge_cases
    h2_client http_out_client http2_out_client biz_http_client http_close_demo http_server_close_demo router_client router_biz_client client_conn_factory_example xproto_server xproto_client ws_bench_client tls_multi_fetch
    demo_multi_protocol_server demo_multi_thread_server h2_server ws_stickbridge_client
    ws_broadcast_user ws_broadcast_periodic framework_ws_server framework_ws_client framework_ws_client_idle
    unified_simple_http unified_mixed_server unified_ws_client_test unified_level2_demo unified_https_wss_server unified_async_demo simple_async_test
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

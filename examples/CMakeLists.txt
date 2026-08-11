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


# 干净 checkout 中部分示例源被 .gitignore 忽略而缺失。mf_example 只注册存在的源，缺失即跳过，
# 保证 clean checkout 也能 cmake 配置成功（无需先备齐全部示例源）。
function(mf_example target src)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${src}")
    add_executable(${target} ${src})
    target_link_libraries(${target} ${COMMON_LIBS})
  else()
    message(STATUS "myframe: skip example '${target}' (missing ${src})")
  endif()
endfunction()
# HTTP Server Example
mf_example(http_server simple_http_server.cpp)

# Level demonstrations
mf_example(level1_multi_protocol level1_multi_protocol.cpp)

mf_example(level2_multi_protocol level2_multi_protocol.cpp)

mf_example(level3_custom_echo level3_custom_echo.cpp)

mf_example(level3_multi_protocol level3_multi_protocol.cpp)

# Thread user-data API demo
mf_example(thread_user_data_demo thread_user_data_demo.cpp)

# Async HTTP Server Demo - showcases async response handling
mf_example(async_http_demo async_http_server_demo.cpp)

# HTTPS Server Example
mf_example(https_server simple_https_server.cpp)

# WebSocket Secure Server Example
mf_example(wss_server simple_wss_server.cpp)


# HTTP/2 Client Example
mf_example(h2_client simple_h2_client.cpp)

# HTTP Outbound Client Example
mf_example(http_out_client http_out_client.cpp)

# TLS multi-fetch demo (HTTPS client sample)
mf_example(tls_multi_fetch tls_multi_fetch.cpp)

# HTTP/2 Outbound Client using out_connect
mf_example(http2_out_client http2_out_client.cpp)

# Minimal business HTTP client example
mf_example(biz_http_client biz_http_client.cpp)

# HTTP graceful close demo
mf_example(http_close_demo http_close_demo.cpp)

# Unified router client for http/https/ws/wss using event loop
mf_example(router_client router_client.cpp)

# Router business client with headers/body and WS handler
mf_example(router_biz_client router_biz_client.cpp)

# ClientConnRouter Example
mf_example(client_conn_factory_example client_conn_factory_example.cpp)

# XPROTO Demo (custom protocol via MultiProtocolFactory + CustomProbe)
mf_example(xproto_server xproto_server.cpp)
mf_example(xproto_client xproto_client.cpp)
# Stock system examples (optional if sources exist)
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/stock_index_server.cpp" AND
    EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/stock_bridge_server.cpp" AND
    EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/stock_driver.cpp")
    mf_example(stock_index_server stock_index_server.cpp)
    mf_example(stock_bridge_server stock_bridge_server.cpp)
    mf_example(stock_driver stock_driver.cpp)
    set(HAVE_STOCK_EXAMPLES TRUE)
else()
    set(HAVE_STOCK_EXAMPLES FALSE)
endif()

# Multi-protocol and multi-threading examples
mf_example(demo_multi_protocol_server multi_protocol_server.cpp)
mf_example(demo_multi_thread_server multi_thread_server.cpp)
# HTTP/2 Server Example (TLS + ALPN h2)
mf_example(h2_server simple_h2_server.cpp)

# HTTP Server Close Demo (graceful close after response)
mf_example(http_server_close_demo http_server_close_demo.cpp)

# WS benchmark client
mf_example(ws_bench_client ws_bench_client.cpp)

# WebSocket broadcast demos
mf_example(ws_broadcast_user ws_broadcast_user.cpp)
mf_example(ws_broadcast_periodic ws_broadcast_periodic.cpp)

# Framework WS smoke-test pair
mf_example(framework_ws_server framework_ws_server.cpp)
mf_example(framework_ws_client framework_ws_client.cpp)
mf_example(framework_ws_client_idle framework_ws_client_idle.cpp)

# StickBridge client demo leveraging myframe websocket stack
mf_example(ws_stickbridge_client ws_stickbridge_client.cpp)

# Unified Protocol Architecture - Simple HTTP Example (Level 1 API)
mf_example(unified_simple_http unified_simple_http.cpp)

# Unified Protocol Architecture - Mixed HTTP + WebSocket Example (Level 1 API)
mf_example(unified_mixed_server unified_mixed_server.cpp)

# Unified Protocol Architecture - WebSocket Client Test
mf_example(unified_ws_client_test unified_ws_client_test.cpp)

# Unified Protocol Architecture - Level 2 Context Demo
mf_example(unified_level2_demo unified_level2_demo.cpp)

# Unified Protocol Architecture - HTTPS/WSS Server Demo
mf_example(unified_https_wss_server unified_https_wss_server.cpp)

# Unified Protocol Architecture - Async Response Demo
mf_example(unified_async_demo unified_async_demo.cpp)

# Simple Async Test with Timer
mf_example(simple_async_test simple_async_test.cpp)

# Test memory management
mf_example(test_memory_management test_memory_management.cpp)

# Test edge cases
mf_example(test_edge_cases test_edge_cases.cpp)

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

# EastMoney xdxr URL repro for stockbridge daemon httpGet diagnosis
mf_example(em_xdxr_test em_xdxr_test.cpp)

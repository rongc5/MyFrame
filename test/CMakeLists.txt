# Test targets (inherits cmake_minimum_required from parent)

# Common include dirs
include_directories(
    ${CMAKE_CURRENT_BINARY_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../core
)

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

# Generate a throwaway localhost identity shared by the TLS test targets.
set(TEST_TLS_DIR "${CMAKE_CURRENT_BINARY_DIR}/certs")
set(TEST_TLS_CERT "${TEST_TLS_DIR}/server.crt")
set(TEST_TLS_KEY "${TEST_TLS_DIR}/server.key")
add_custom_command(OUTPUT "${TEST_TLS_CERT}" "${TEST_TLS_KEY}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${TEST_TLS_DIR}"
    COMMAND openssl req -x509 -newkey rsa:2048 -nodes
            -keyout "${TEST_TLS_KEY}" -out "${TEST_TLS_CERT}"
            -days 30 -subj /CN=localhost
    VERBATIM)
add_custom_target(myframe_test_certs DEPENDS "${TEST_TLS_CERT}" "${TEST_TLS_KEY}")

function(use_test_tls target)
    add_dependencies(${target} myframe_test_certs)
    target_compile_definitions(${target} PRIVATE
        MYFRAME_TEST_CERT="${TEST_TLS_CERT}" MYFRAME_TEST_KEY="${TEST_TLS_KEY}")
endfunction()

# Helper to add executable if source exists (CMake 2.8 compatible)
function(maybe_add_exe name src)
    if (EXISTS "${src}")
        add_executable(${name} ${src})
        # Tests verify results with assert(): keep it active in Release builds.
        target_compile_options(${name} PRIVATE -UNDEBUG)
        target_link_libraries(${name} myframe ${OPENSSL_LIBRARIES} ${CMAKE_THREAD_LIBS_INIT})
        set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/test)
    endif()
endfunction()

maybe_add_exe(multi_protocol_server ${CMAKE_CURRENT_SOURCE_DIR}/multi_protocol_server.cpp)
# client.cpp 依赖私有CommonMsg定义，默认不构建
maybe_add_exe(test_unit ${CMAKE_CURRENT_SOURCE_DIR}/test_unit.cpp)
# Regression: header OWS trim + case-insensitive get_header
maybe_add_exe(test_http_header ${CMAKE_CURRENT_SOURCE_DIR}/test_http_header.cpp)
if(TARGET test_http_header)
    add_test(NAME http_header COMMAND test_http_header)
endif()
maybe_add_exe(test_http_content_length_fragments ${CMAKE_CURRENT_SOURCE_DIR}/test_http_content_length_fragments.cpp)
if(TARGET test_http_content_length_fragments)
    add_test(NAME http_content_length_fragments COMMAND test_http_content_length_fragments)
endif()
# Level-1 HEAD transport semantics (length preservation + body suppression).
maybe_add_exe(test_http_head ${CMAKE_CURRENT_SOURCE_DIR}/test_http_head.cpp)
if(TARGET test_http_head)
    add_test(NAME http_head COMMAND test_http_head)
    set_tests_properties(http_head PROPERTIES TIMEOUT 30)
endif()
maybe_add_exe(test_https_head ${CMAKE_CURRENT_SOURCE_DIR}/test_https_head.cpp)
if(TARGET test_https_head)
    use_test_tls(test_https_head)
    add_test(NAME https_head COMMAND test_https_head)
    set_tests_properties(https_head PROPERTIES TIMEOUT 30)
endif()
maybe_add_exe(test_http_context ${CMAKE_CURRENT_SOURCE_DIR}/test_http_context.cpp)
if(TARGET test_http_context)
    add_test(NAME http_context COMMAND test_http_context)
endif()
maybe_add_exe(test_task_executor ${CMAKE_CURRENT_SOURCE_DIR}/test_task_executor.cpp)
if(TARGET test_task_executor)
    add_test(NAME task_executor COMMAND test_task_executor)
endif()
# Router 接入分发集成（route_param/HEAD→GET 抑制 body/auto-OPTIONS/404/405/400）
maybe_add_exe(test_http_routing ${CMAKE_CURRENT_SOURCE_DIR}/test_http_routing.cpp)
if(TARGET test_http_routing)
    add_test(NAME http_routing COMMAND test_http_routing)
    set_tests_properties(http_routing PROPERTIES TIMEOUT 60)
endif()
# 中间件链回归（before 顺序/after 逆序/短路/异常/after-once）
maybe_add_exe(test_http_middleware ${CMAKE_CURRENT_SOURCE_DIR}/test_http_middleware.cpp)
if(TARGET test_http_middleware)
    add_test(NAME http_middleware COMMAND test_http_middleware)
endif()
# 响应侧 JSON 便捷回归
maybe_add_exe(test_http_json ${CMAKE_CURRENT_SOURCE_DIR}/test_http_json.cpp)
if(TARGET test_http_json)
    add_test(NAME http_json COMMAND test_http_json)
endif()
# 路由表回归（静态优先/{param}/decode/405 Allow/freeze/并发只读）
maybe_add_exe(test_http_router ${CMAKE_CURRENT_SOURCE_DIR}/test_http_router.cpp)
if(TARGET test_http_router)
    add_test(NAME http_router COMMAND test_http_router)
endif()
# 真实 HTTP 服务上的两阶段异步生命周期集成测试（成功/500/503/断开/Keep-Alive body）
maybe_add_exe(test_http_async_integration ${CMAKE_CURRENT_SOURCE_DIR}/test_http_async_integration.cpp)
if(TARGET test_http_async_integration)
    add_test(NAME http_async_integration COMMAND test_http_async_integration)
    set_tests_properties(http_async_integration PROPERTIES TIMEOUT 60)
endif()
maybe_add_exe(test_business_handler ${CMAKE_CURRENT_SOURCE_DIR}/test_business_handler.cpp)
# Regression test for channel_data_process watchdog routing + obj_process unconditional dispatch.
maybe_add_exe(test_channel_watchdog ${CMAKE_CURRENT_SOURCE_DIR}/test_channel_watchdog.cpp)
# Accept fairness, per-server stop/join isolation, and channel wakeup/FD lifecycle.
maybe_add_exe(test_server_lifecycle ${CMAKE_CURRENT_SOURCE_DIR}/test_server_lifecycle.cpp)
if(TARGET test_server_lifecycle)
    foreach(case accept_backlog accept_burst accept_emfile server_isolation idempotent_stop_join channel_coalescing channel_fd_teardown)
        add_test(NAME lifecycle_${case} COMMAND test_server_lifecycle ${case})
        set_tests_properties(lifecycle_${case} PROPERTIES TIMEOUT 60)
    endforeach()
endif()
# High-concurrency exact connection and message accounting stress test.
maybe_add_exe(test_high_concurrency_stress ${CMAKE_CURRENT_SOURCE_DIR}/test_high_concurrency_stress.cpp)
if(TARGET test_high_concurrency_stress)
    add_test(NAME high_concurrency_stress COMMAND test_high_concurrency_stress)
    set_tests_properties(high_concurrency_stress PROPERTIES TIMEOUT 90)
endif()
# P0: fd 0 / -1 sentinel safety, idempotent destroy, outbound accounting.
maybe_add_exe(test_fd_sentinel ${CMAKE_CURRENT_SOURCE_DIR}/test_fd_sentinel.cpp)
if(TARGET test_fd_sentinel)
    add_test(NAME fd_sentinel COMMAND test_fd_sentinel)
    set_tests_properties(fd_sentinel PROPERTIES TIMEOUT 30)
endif()
# P0: concurrent WsPushHub broadcast during client connect/disconnect churn.
maybe_add_exe(test_ws_broadcast_churn ${CMAKE_CURRENT_SOURCE_DIR}/test_ws_broadcast_churn.cpp)
if(TARGET test_ws_broadcast_churn)
    add_test(NAME ws_broadcast_churn COMMAND test_ws_broadcast_churn)
    set_tests_properties(ws_broadcast_churn PROPERTIES TIMEOUT 180)
endif()
# P0: slow-consumer backpressure (stalled receiver, bounded RSS).
maybe_add_exe(test_ws_slow_consumer ${CMAKE_CURRENT_SOURCE_DIR}/test_ws_slow_consumer.cpp)
if(TARGET test_ws_slow_consumer)
    add_test(NAME ws_slow_consumer COMMAND test_ws_slow_consumer)
    set_tests_properties(ws_slow_consumer PROPERTIES TIMEOUT 180)
endif()
# P1: monotonic timer, lock-free put_obj_msg stress, teardown, graceful drain.
maybe_add_exe(test_p1_core ${CMAKE_CURRENT_SOURCE_DIR}/test_p1_core.cpp)
if(TARGET test_p1_core)
    add_test(NAME p1_core COMMAND test_p1_core)
    set_tests_properties(p1_core PROPERTIES TIMEOUT 400)
endif()

maybe_add_exe(test_http_persistence ${CMAKE_CURRENT_SOURCE_DIR}/test_http_persistence.cpp)
if(TARGET test_http_persistence)
    add_test(NAME http_persistence COMMAND test_http_persistence)
    set_tests_properties(http_persistence PROPERTIES TIMEOUT 15)
endif()

# Large-transfer, TLS, header-guard and concurrency stress regression.
maybe_add_exe(test_large_transfer_and_stress ${CMAKE_CURRENT_SOURCE_DIR}/test_large_transfer_and_stress.cpp)
if(TARGET test_large_transfer_and_stress)
    use_test_tls(test_large_transfer_and_stress)
    add_test(NAME large_transfer_and_stress COMMAND test_large_transfer_and_stress)
    set_tests_properties(large_transfer_and_stress PROPERTIES TIMEOUT 180)
endif()

# Batch 1 security / framing regression suite (HTTP smuggling, chunked
# overflow, keep-alive status line, detector handoff, binary TLV, WebSocket
# hardening, TLS protocol range, HTTP/2 split frames, event-loop mutation).
maybe_add_exe(test_security_and_framing ${CMAKE_CURRENT_SOURCE_DIR}/test_security_and_framing.cpp)
if(TARGET test_security_and_framing)
    use_test_tls(test_security_and_framing)
    add_test(NAME security_and_framing COMMAND test_security_and_framing)
    set_tests_properties(security_and_framing PROPERTIES TIMEOUT 120)
endif()

# Cryptography wrapper suite (NIST/RFC vectors for AES-GCM/CBC/CTR, SHA/HMAC,
# HKDF/PBKDF2, Base64/Hex, CSPRNG, RSA-OAEP/PSS, ECDSA-P256, Ed25519).
maybe_add_exe(test_crypto ${CMAKE_CURRENT_SOURCE_DIR}/test_crypto.cpp)
if(TARGET test_crypto)
    add_test(NAME crypto COMMAND test_crypto)
    set_tests_properties(crypto PROPERTIES TIMEOUT 60)
endif()

# Lock-free queues suite (SpscQueue + Vyukov MpmcQueue: unit, lifetime,
# exception injection, SIZE_MAX wrap-around, and exact-accounting concurrency).
maybe_add_exe(test_lockfree_queue ${CMAKE_CURRENT_SOURCE_DIR}/test_lockfree_queue.cpp)
if(TARGET test_lockfree_queue)
    target_compile_options(test_lockfree_queue PRIVATE -Wall -Wextra -Werror -Wpedantic -Wshadow)
    add_test(NAME lockfree_queue COMMAND test_lockfree_queue)
    set_tests_properties(lockfree_queue PROPERTIES TIMEOUT 180)
endif()

# Utility containers suite (STX B+ Tree include fix, LruCache, ShardedMap).
maybe_add_exe(test_utils_containers ${CMAKE_CURRENT_SOURCE_DIR}/test_utils_containers.cpp)
if(TARGET test_utils_containers)
    target_compile_options(test_utils_containers PRIVATE -Wall -Wextra -Werror -Wpedantic -Wshadow)
    add_test(NAME utils_containers COMMAND test_utils_containers)
    set_tests_properties(utils_containers PROPERTIES TIMEOUT 120)
endif()

# Ads & search backend data structures suite (RadixTree, SuffixMatcher,
# AhoCorasick, BloomFilter, InvertedIndex, TopK, ShardedTtlCache,
# ConsistentHash, TokenBucket/ShardedRateLimiter, DoubleBuffer).
maybe_add_exe(test_search_and_ads_utils ${CMAKE_CURRENT_SOURCE_DIR}/test_search_and_ads_utils.cpp)
if(TARGET test_search_and_ads_utils)
    target_compile_options(test_search_and_ads_utils PRIVATE -Wall -Wextra -Werror -Wpedantic -Wshadow)
    add_test(NAME search_and_ads_utils COMMAND test_search_and_ads_utils)
    set_tests_properties(search_and_ads_utils PROPERTIES TIMEOUT 180)
endif()

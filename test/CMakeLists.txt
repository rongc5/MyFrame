# Test targets (inherits cmake_minimum_required from parent)

# Common include dirs
include_directories(
    ${CMAKE_CURRENT_BINARY_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../core
)

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

# Helper to add executable if source exists (CMake 2.8 compatible)
function(maybe_add_exe name src)
    if (EXISTS "${src}")
        add_executable(${name} ${src})
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

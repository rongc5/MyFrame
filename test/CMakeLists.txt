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
maybe_add_exe(test_business_handler ${CMAKE_CURRENT_SOURCE_DIR}/test_business_handler.cpp)
# Regression test for channel_data_process watchdog routing + obj_process unconditional dispatch.
maybe_add_exe(test_channel_watchdog ${CMAKE_CURRENT_SOURCE_DIR}/test_channel_watchdog.cpp)

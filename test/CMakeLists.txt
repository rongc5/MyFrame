cmake_minimum_required(VERSION 3.16)

# Common include dirs
include_directories(
    ${CMAKE_CURRENT_BINARY_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

# Helper to add executable if source exists
function(maybe_add_exe name src)
    if (EXISTS "${src}")
        add_executable(${name} ${src})
        target_link_libraries(${name} myframe OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
        set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/test)
    endif()
endfunction()

maybe_add_exe(multi_protocol_server ${CMAKE_CURRENT_SOURCE_DIR}/multi_protocol_server.cpp)
# client.cpp 依赖私有CommonMsg定义，默认不构建

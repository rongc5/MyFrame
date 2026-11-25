cmake_minimum_required(VERSION 3.10)
project(MyFrame CXX)

# C++ standard: C++11 by default. Downstream builds may raise it (for example
# to match the ABI of a C++17 consumer) via -DMYFRAME_CXX_STANDARD=14 or 17.
set(MYFRAME_CXX_STANDARD "11" CACHE STRING "C++ standard for MyFrame (11, 14, or 17)")
set_property(CACHE MYFRAME_CXX_STANDARD PROPERTY STRINGS 11 14 17)
set(CMAKE_CXX_STANDARD ${MYFRAME_CXX_STANDARD})
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Debug logging option (controls PDEBUG)
option(MYFRAME_DEBUG_LOG "Enable verbose PDEBUG logging" OFF)
if (MYFRAME_DEBUG_LOG)
  add_definitions(-DDEBUG)
endif()

# Enable SSL support
add_definitions(-DENABLE_SSL)

# Find OpenSSL
find_package(OpenSSL REQUIRED)
if (OPENSSL_FOUND)
    include_directories(${OPENSSL_INCLUDE_DIR})
endif()

# Find Threads
find_package(Threads REQUIRED)
find_package(ZLIB)
if (ZLIB_FOUND)
    include_directories(${ZLIB_INCLUDE_DIRS})
endif()

## Include directories
include_directories(include)
include_directories(utils)
include_directories(SYSTEM utils/btree)
include_directories(utils/encode)
include_directories(utils/rapidjson)

## Native utility libraries (built fresh into the build tree; never rely on
## prebuilt archives in the source tree so cross-architecture builds work).
set(ENCODE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/utils/encode)
set(STR2SIGN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/utils/str2sign)

if (NOT EXISTS "${ENCODE_DIR}/des.cpp" OR NOT EXISTS "${ENCODE_DIR}/uconv.cpp")
    message(FATAL_ERROR "utils/encode sources (des.cpp, uconv.cpp) are missing")
endif()
add_library(myframe_encode STATIC
    ${ENCODE_DIR}/des.cpp
    ${ENCODE_DIR}/uconv.cpp
)
target_include_directories(myframe_encode PUBLIC ${ENCODE_DIR})
set_target_properties(myframe_encode PROPERTIES
    OUTPUT_NAME encode
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

if (EXISTS "${STR2SIGN_DIR}/core/ul_sign.cpp" AND
    EXISTS "${STR2SIGN_DIR}/core/ul_prime.cpp" AND
    EXISTS "${STR2SIGN_DIR}/core/ul_sign.h" AND
    EXISTS "${STR2SIGN_DIR}/core/ul_prime.h")
    add_library(myframe_sign STATIC
        ${STR2SIGN_DIR}/core/ul_prime.cpp
        ${STR2SIGN_DIR}/core/ul_sign.cpp
    )
    target_include_directories(myframe_sign PUBLIC ${STR2SIGN_DIR}/core)
    set_target_properties(myframe_sign PROPERTIES
        OUTPUT_NAME sign
        ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
    set(HAVE_SIGN_LIB TRUE)
    message(STATUS "Building native str2sign library (myframe_sign)")
else()
    set(HAVE_SIGN_LIB FALSE)
    message(WARNING "utils/str2sign sources not found; building with fallback hash implementation")
endif()

# Compatibility copies for legacy consumers that link against
# utils/encode/lib/libencode.a or utils/str2sign/lib/libsign.a. Off by
# default so a build never writes into the source tree.
option(MYFRAME_LEGACY_UTILS_COPY "Copy libencode.a/libsign.a and headers into utils/*/lib and utils/*/include after building" OFF)

## Core source files (minimal set)
file(GLOB CORE_SOURCES
    "core/*.cpp"
    "core/protocol_adapters/*.cpp"
    "crypto/*.cpp"
)
# Exclude legacy logging modules that require additional infra
## Exclude legacy log modules; provide stubs instead
list(REMOVE_ITEM CORE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/core/log_process.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/core/log_thread.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/core/log_base.cpp"
)
## Replace non-UTF8 client_conn_router.cpp with clean version client_conn_router2.cpp
list(REMOVE_ITEM CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/core/client_conn_router.cpp")
## Exclude old non-UTF8 http2_client implementation; replaced by http2_client_alpn.cpp
list(REMOVE_ITEM CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/core/http2_client.cpp")
list(REMOVE_ITEM CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/core/connection_manager.cpp")

# Create library
add_library(myframe STATIC ${CORE_SOURCES})
target_include_directories(myframe PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/utils
)
target_include_directories(myframe SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/utils/btree)
target_link_libraries(myframe
    myframe_encode
    ${CMAKE_THREAD_LIBS_INIT}
    ${OPENSSL_LIBRARIES}
)
if (ZLIB_FOUND)
    target_link_libraries(myframe ${ZLIB_LIBRARIES})
    add_definitions(-DHAVE_ZLIB)
endif()
if (HAVE_SIGN_LIB)
    # HAVE_UL_SIGN_H is defined only when myframe_sign is built and linked.
    target_link_libraries(myframe myframe_sign)
    target_compile_definitions(myframe PUBLIC HAVE_UL_SIGN_H)
    target_include_directories(myframe PUBLIC ${STR2SIGN_DIR}/core)
endif()

if (MYFRAME_LEGACY_UTILS_COPY)
    add_custom_command(TARGET myframe_encode POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory ${ENCODE_DIR}/lib
        COMMAND ${CMAKE_COMMAND} -E make_directory ${ENCODE_DIR}/include
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:myframe_encode> ${ENCODE_DIR}/lib/libencode.a
        COMMAND ${CMAKE_COMMAND} -E copy
            ${ENCODE_DIR}/des.h ${ENCODE_DIR}/uconv.h ${ENCODE_DIR}/cp936map.h
            ${ENCODE_DIR}/psedoutf8.h ${ENCODE_DIR}/un2gbkmap.h
            ${ENCODE_DIR}/include/
        VERBATIM)
    if (HAVE_SIGN_LIB)
        add_custom_command(TARGET myframe_sign POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory ${STR2SIGN_DIR}/lib
            COMMAND ${CMAKE_COMMAND} -E make_directory ${STR2SIGN_DIR}/include
            COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:myframe_sign> ${STR2SIGN_DIR}/lib/libsign.a
            COMMAND ${CMAKE_COMMAND} -E copy
                ${STR2SIGN_DIR}/core/ul_sign.h ${STR2SIGN_DIR}/core/ul_prime.h
                ${STR2SIGN_DIR}/include/
            VERBATIM)
    endif()
endif()


# Copy headers to include directory in build folder
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/include)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/include/protocol_adapters)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/include/crypto)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/include/lockfree)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/include/containers)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/include/stx)
file(COPY core/ DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/include
     FILES_MATCHING PATTERN "*.h")
file(COPY crypto/ DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/include/crypto
     FILES_MATCHING PATTERN "*.h")
file(COPY utils/lockfree/ DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/include/lockfree
     FILES_MATCHING PATTERN "*.h")
file(COPY utils/containers/ DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/include/containers
     FILES_MATCHING PATTERN "*.h")
file(COPY utils/btree/stx/ DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/include/stx)
file(REMOVE "${CMAKE_CURRENT_BINARY_DIR}/include/app_handler.h")
if (HAVE_SIGN_LIB)
    file(COPY ${STR2SIGN_DIR}/core/ DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/include
         FILES_MATCHING PATTERN "*.h")
endif()
file(COPY ${ENCODE_DIR}/ DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/include
     FILES_MATCHING PATTERN "*.h"
     PATTERN "include" EXCLUDE PATTERN "lib" EXCLUDE PATTERN "core" EXCLUDE)

# Also expose headers to consumers
include_directories(${CMAKE_CURRENT_BINARY_DIR}/include)

# Create lib directory and copy library
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/lib)
add_custom_command(TARGET myframe POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:myframe> ${CMAKE_CURRENT_BINARY_DIR}/lib/
    VERBATIM
)

# Test targets - optional
enable_testing()   # Enable CTest so add_test() calls in test/ take effect.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test/CMakeLists.txt")
    add_subdirectory(test)
endif()

# Examples targets - optional, OFF by default: some example sources are ignored
# by .gitignore and a clean checkout must still build the library. Enable with
# -DMYFRAME_BUILD_EXAMPLES=ON (examples/CMakeLists.txt skips missing sources).
option(MYFRAME_BUILD_EXAMPLES "Build example programs" OFF)
if(MYFRAME_BUILD_EXAMPLES AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/CMakeLists.txt")
    add_subdirectory(examples)
endif()

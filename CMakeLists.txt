cmake_minimum_required(VERSION 3.16)
project(MyFrame)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Enable SSL support
add_definitions(-DENABLE_SSL)

# Find OpenSSL
find_package(OpenSSL REQUIRED)

# Find Threads
find_package(Threads REQUIRED)

## Include directories
include_directories(include)
include_directories(utils/btree)
include_directories(utils/encode)
include_directories(utils/rapidjson)
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/utils/str2sign/include/ul_sign.h")
    include_directories(utils/str2sign/include)
    add_definitions(-DHAVE_UL_SIGN_H)
else()
    message(WARNING "utils/str2sign/include/ul_sign.h not found; building with fallback hash implementation")
endif()


## Core source files (minimal set)
file(GLOB CORE_SOURCES
    "core/*.cpp"
)
# Exclude legacy logging modules that require additional infra
## Exclude legacy log modules; provide stubs instead
list(REMOVE_ITEM CORE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/core/log_process.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/core/log_thread.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/core/log_base.cpp"
)
list(REMOVE_ITEM CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/core/connection_manager.cpp")

## Ensure utils/encode is built (optional signer is handled below)
add_custom_target(build_encode ALL
    COMMAND bash -lc "./build.sh all" 
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/utils/encode
)

## Optional utils/str2sign (header+static lib if present)
set(STR2SIGN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/utils/str2sign)
set(SIGN_HEADER ${STR2SIGN_DIR}/include/ul_sign.h)
set(SIGN_LIB    ${STR2SIGN_DIR}/lib/libsign.a)
if (EXISTS "${SIGN_HEADER}" AND EXISTS "${SIGN_LIB}")
    set(HAVE_SIGN_LIB TRUE)
else()
    message(WARNING "utils/str2sign not fully available (need include/ul_sign.h and lib/libsign.a); skipping sign library link")
    set(HAVE_SIGN_LIB FALSE)
endif()

# Create library
add_library(myframe STATIC ${CORE_SOURCES})
add_dependencies(myframe build_encode)

# Link libraries 
target_link_libraries(myframe 
    ${CMAKE_CURRENT_SOURCE_DIR}/utils/encode/lib/libencode.a
    Threads::Threads 
    OpenSSL::SSL 
    OpenSSL::Crypto
)
if (HAVE_SIGN_LIB)
    target_link_libraries(myframe ${CMAKE_CURRENT_SOURCE_DIR}/utils/str2sign/lib/libsign.a)
endif()


# Copy headers to include directory in build folder
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/include)
file(COPY core/ DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/include
     FILES_MATCHING PATTERN "*.h")
if (HAVE_SIGN_LIB AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/utils/str2sign/include")
    file(COPY utils/str2sign/include/ DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/include
         FILES_MATCHING PATTERN "*.h")
endif()
file(COPY utils/encode/include/ DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/include
     FILES_MATCHING PATTERN "*.h")

# Also expose headers to consumers
include_directories(${CMAKE_CURRENT_BINARY_DIR}/include)

# Create lib directory and copy library
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/lib)
add_custom_command(TARGET myframe POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:myframe> ${CMAKE_CURRENT_BINARY_DIR}/lib/
)

# Test targets - optional
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test/CMakeLists.txt")
    add_subdirectory(test)
endif()

# Examples targets - optional
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/CMakeLists.txt")
    add_subdirectory(examples)
endif()

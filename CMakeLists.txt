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

# Include directories
include_directories(include)
include_directories(utils/btree)
include_directories(utils/encode)
include_directories(utils/rapidjson)
include_directories(utils/str2sign/include)


# Core source files (exclude base_thread.cpp as it's merged into base_net_thread)
file(GLOB CORE_SOURCES 
    "core/*.cpp"
)
list(REMOVE_ITEM CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/core/base_thread.cpp")

# Ensure utils/str2sign and utils/encode are built
add_custom_target(build_sign ALL
    COMMAND bash -lc "./build.sh all"
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/utils/str2sign
)

add_custom_target(build_encode ALL
    COMMAND bash -lc "./build.sh all" 
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/utils/encode
)

# Create library
add_library(myframe STATIC ${CORE_SOURCES})
add_dependencies(myframe build_sign build_encode)

# Link libraries 
target_link_libraries(myframe 
    ${CMAKE_CURRENT_SOURCE_DIR}/utils/str2sign/lib/libsign.a
    ${CMAKE_CURRENT_SOURCE_DIR}/utils/encode/lib/libencode.a
    Threads::Threads 
    OpenSSL::SSL 
    OpenSSL::Crypto
)


# Copy headers to include directory in build folder
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/include)
file(COPY core/ DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/include
     FILES_MATCHING PATTERN "*.h")
file(COPY utils/str2sign/include/ DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/include
     FILES_MATCHING PATTERN "*.h")
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

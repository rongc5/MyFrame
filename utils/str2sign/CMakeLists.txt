# str2sign utility library
cmake_minimum_required(VERSION 3.16)

# Source files
file(GLOB STR2SIGN_SOURCES "core/*.cpp")

# Create static library
add_library(sign STATIC ${STR2SIGN_SOURCES})

# Include directories
target_include_directories(sign PUBLIC include core)
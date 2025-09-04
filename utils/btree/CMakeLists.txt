# Btree - Header-only library

# Collect all header files
file(GLOB BTREE_HEADERS "*.h" "btree*")

# Create interface library for header-only library
add_library(btree INTERFACE)

target_include_directories(btree INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

# Install headers
install(FILES ${BTREE_HEADERS} DESTINATION include/btree)
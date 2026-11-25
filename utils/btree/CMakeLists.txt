# STX B+ Tree - Header-only library

file(GLOB BTREE_SHIM_HEADERS "*.h" "btree" "btree_map" "btree_multimap" "btree_set" "btree_multiset")
file(GLOB BTREE_STX_HEADERS "stx/*")

add_library(btree INTERFACE)
target_include_directories(btree SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

install(FILES ${BTREE_SHIM_HEADERS} DESTINATION include/btree)
install(FILES ${BTREE_STX_HEADERS} DESTINATION include/btree/stx)

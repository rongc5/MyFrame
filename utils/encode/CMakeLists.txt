# Encode utility library

set(ENCODE_SOURCES
    des.cpp
    uconv.cpp
)

set(ENCODE_HEADERS
    des.h
    uconv.h
    cp936map.h
    psedoutf8.h
    un2gbkmap.h
)

add_library(encode STATIC ${ENCODE_SOURCES})

target_include_directories(encode PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Install headers
install(FILES ${ENCODE_HEADERS} DESTINATION include/encode)
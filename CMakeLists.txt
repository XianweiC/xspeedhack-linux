cmake_minimum_required(VERSION 3.18)
project(xspeedhack_linux C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(xspeedhack SHARED cpp/speedhack.c)
set_target_properties(xspeedhack PROPERTIES
    OUTPUT_NAME "xspeedhack"
    POSITION_INDEPENDENT_CODE ON
)

target_compile_definitions(xspeedhack PRIVATE _GNU_SOURCE)

target_link_libraries(xspeedhack PRIVATE dl pthread)
find_library(RT_LIB rt)
if(RT_LIB)
    target_link_libraries(xspeedhack PRIVATE ${RT_LIB})
endif()

install(TARGETS xspeedhack
    LIBRARY DESTINATION xspeedhack_linux/bin
)

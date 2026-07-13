include (${CMAKE_MODULE_PATH}/sqlite3.cmake)
if (NOT SQLITE3_FOUND)
    message(STATUS "sqlite3 not found, not building hspgo")
    return()
endif()

include_directories(SYSTEM ${SQLITE3_INCLUDE_DIRS})
include_directories(${PROJECT_SOURCE_DIR}/tools/hsbench)

set(hspgo_SOURCES
    main.cpp
    ../hsbench/data_corpus.cpp
    ../hsbench/data_corpus.h
    ../hsbench/heapstats.cpp
    ../hsbench/heapstats.h
)

set(hspgo_INTERNAL_OBJECTS)
if (BUILD_STATIC_AND_SHARED OR BUILD_SHARED_LIBS)
    list(APPEND hspgo_INTERNAL_OBJECTS $<TARGET_OBJECTS:hs_compile_shared>)
    list(APPEND hspgo_INTERNAL_OBJECTS $<TARGET_OBJECTS:hs_exec_shared>)
endif()

add_executable(hspgo ${hspgo_SOURCES} ${hspgo_INTERNAL_OBJECTS})
target_link_libraries(hspgo expressionutil hs ${SQLITE3_LDFLAGS}
    ${CMAKE_THREAD_LIBS_INIT})

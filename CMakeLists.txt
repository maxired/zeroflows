
cmake_minimum_required (VERSION 2.6)
project(Zero-Flows C)



####################################
### Environment and dependencies ###
####################################

include(CheckTypeSize)
include(CheckIncludeFile)
include(CheckLibraryExists)
find_package(PkgConfig)

pkg_check_modules(GLIB2 REQUIRED glib-2.0 gthread-2.0 gmodule-2.0)
pkg_check_modules(JANSSON REQUIRED jansson)
pkg_check_modules(ZMQ REQUIRED libzmq)

CHECK_TYPE_SIZE(long SIZEOF_LONG)
if(SIZEOF_LONG EQUAL 8)
    set (LD_LIBDIR "lib64")
else(SIZEOF_LONG EQUAL 8)
    set (LD_LIBDIR "lib")
endif(SIZEOF_LONG EQUAL 8)

macro(test_FOUND var msg)
    if (${var})
        message(STATUS "FOUND ${msg} : ${${var}}")
    else (${var})
        message(FATAL_ERROR "NOT FOUND ${msg}")
    endif (${var})
    unset(${var} CACHE)
endmacro(test_FOUND)

set(ZK_INCLUDE_DIRS "/usr/include/zookeeper")
set(ZK_LIBRARY_DIRS "/usr/${LD_LIBDIR}")
if (ZK_INCDIR)
    set(ZK_INCLUDE_DIRS ${ZK_INCDIR})
endif(ZK_INCDIR)
if(ZK_LIBDIR)
    set(ZK_LIBRARY_DIRS ${ZK_LIBDIR})
endif(ZK_LIBDIR)
find_file(FOUND zookeeper/zookeeper.h ${ZK_INCLUDE_DIRS})
test_FOUND(FOUND "ZooKeeper header not found")
find_library(ZK_LIBRARIES libzookeeper_st.so ${ZK_LIBRARY_DIRS})



##########################
### Build instructions ###
##########################

set(CMAKE_C_FLAGS "-g -pipe -Wall -Wextra -std=c99")
set(CMAKE_C_FLAGS_DEBUG          "-O0 -fno-inline")
set(CMAKE_C_FLAGS_RELWITHDEBINFO "-O2 -fno-inline")
set(CMAKE_C_FLAGS_RELEASE        "-O2")
set(CMAKE_C_FLAGS_MINSIZEREL     "-Os -s")

if (NOT CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
    add_definitions(-DHAVE_ASSERT=1)
endif()

add_definitions(-D_XOPEN_SOURCE=700)
add_definitions(-D_REENTRANT)
add_definitions(-D_LARGE_FILES)
add_definitions(-D_LARGEFILE_SOURCE)
add_definitions(-D_LARGEFILE64_SOURCE)
add_definitions(-D_FILE_OFFSET_BITS=64)
add_definitions(-DG_ERRORCHECK_MUTEXES)
add_definitions(-DG_DISABLE_DEPRECATED=1)

include_directories(BEFORE .)
include_directories(AFTER
        ${ZMQ_INCLUDE_DIRS}
        ${ZK_INCLUDE_DIRS}/zookeeper
        ${GLIB2_INCLUDE_DIRS})

link_directories(
        ${ZMQ_LIBRARY_DIRS}
        ${ZK_LIBRARY_DIRS})

add_library(zsock SHARED 
        zservice.c zsock.c zsock_config.c zutils.c zsock.h
        zreactor.c zreactor.h
        macros.h)
target_link_libraries(zsock
        ${ZMQ_LIBRARIES}
        ${JANSSON_LIBRARIES}
        ${ZK_LIBRARIES}
        ${GLIB2_LIBRARIES})

add_library(main_utils STATIC
        common.c common.h
        zsock.h zreactor.h
        macros.h)
target_link_libraries(main_utils zsock
        ${GLIB2_LIBRARIES})

add_executable(zservice main_service.c macros.h zsock.h zreactor.h)
target_link_libraries(zservice zsock main_utils)

add_executable(zpipe main_pipe.c macros.h zsock.h zreactor.h)
target_link_libraries(zpipe zsock main_utils)

install(TARGETS zsock zservice zpipe
        LIBRARY DESTINATION ${LD_LIBDIR}
        ARCHIVE DESTINATION ${LD_LIBDIR}
        RUNTIME DESTINATION bin)


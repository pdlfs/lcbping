#
# CMakeLists.txt  cmake for lcbping
# 13-Jul-2022  chuck@ece.cmu.edu
#

#
# leveled call back ping program.  does a simple stop-and-wait ping
# between two endpoints at various levels in the I/O stack.  LCB_*
# variables can be set on the cmake command line with -D.
#
# supported level types:
#  LCB_MERCURY - adds "hg" (RPC) and "na" types (CHECK, ON, OFF)
#  LCB_PSM - adds "psm" type (CHECK, ON, OFF)
#
# values: CHECK=enable if found (the default),
#         ON=force on and fail if not found
#         OFF=force off even if present on system
#
# additional options:
#  LCB_MPI - support address exchanges using MPI (ON, OFF - default)
#            to use: run 2 procs with MPI and use 'MPI' as addr-spec
#  LCB_HG_DLOG - have hg plugin keep a dlog.  set environment variable
#                HGCB_DLOG_DUMP to dump dlog when program exits.
#
# specify location(s) of plugin support libs with CMAKE_INSTALL_PATH
# and/or CMAKE_PREFIX_PATH.
#
# we install binary in CMAKE_INSTALL_PREFIX
#
# require cmake 3.9 (july 2017)... the oldest version of cmake that
# supports MPI with imported targets.
#

cmake_minimum_required(VERSION 3.9)
project(lcbping C)

# add our cmake module directory to the path
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# avoid the need to set LD_LIBRARY_PATH with the installed binary
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)

# setup cache variables for ccmake
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release
        CACHE STRING "Choose the type of build." FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
                 "Debug" "Release" "RelWithDebInfo" "MinSizeRel")
endif()

set(LCB_MERCURY CHECK CACHE STRING "Build Mercury hg and na plugins")
set_property(CACHE LCB_MERCURY PROPERTY STRINGS CHECK ON OFF)

set(LCB_PSM CHECK CACHE STRING "Build PSM plugin")
set_property(CACHE LCB_PSM PROPERTY STRINGS CHECK ON OFF)

# now bring in the packages we need and ones requested by the user
find_package(Threads REQUIRED)    # we need the Threads::Threads target

if(LCB_MERCURY)
    find_package(mercury CONFIG)
    if(NOT TARGET mercury AND NOT LCB_MERCURY MATCHES "CHECK|check")
        message(FATAL_ERROR "LCB_MERCURY set and mercury target not found")
    endif()
endif()
if(LCB_PSM)
    find_package(psm MODULE)
    if(NOT TARGET psm AND NOT LCB_PSM MATCHES "CHECK|check")
        message(FATAL_ERROR "LCB_PSM set and psm target not found")
    endif()
endif()
if(LCB_PSM2)
    find_package(psm2 MODULE)
    if(NOT TARGET psm2 AND NOT LCB_PSM2 MATCHES "CHECK|check")
        message(FATAL_ERROR "LCB_PSM2 set and psm2 target not found")
    endif()
endif()

set(LCB_MPI "OFF" CACHE BOOL "Enable MPI-based address exchange")
if (LCB_MPI)
    find_package(MPI MODULE REQUIRED)
endif()

set(LCB_HG_DLOG "OFF" CACHE BOOL "Enable DLOG code in hg plugin")

add_subdirectory(src)

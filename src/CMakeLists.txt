#
# CMakeLists.txt  cmake for lcbping/src
# 13-Jul-2022  chuck@ece.cmu.edu
#

set(lcbsrc cb.c lcbping.c)
unset(targets)

if(TARGET mercury)
    list(APPEND lcbsrc cb_hg.c cb_na.c)
    list(APPEND targets mercury)
    add_compile_definitions(LCB_MERCURY)
    message(STATUS "mercury hg and na cb backends enabled")
    if(LCB_HG_DLOG)
        add_compile_definitions(LCB_HG_DLOG)
        message(STATUS "hg DLOG code enabled")
    endif()
endif()

if(TARGET psm)
    list(APPEND lcbsrc cb_psm.c)
    list(APPEND targets psm)
    add_compile_definitions(LCB_PSM)
    message(STATUS "psm backend enabled")
endif()

if(TARGET MPI::MPI_C)
    list(APPEND targets MPI::MPI_C)
    add_compile_definitions(LCB_MPI)
    message(STATUS "MPI address exchange support enabled")
endif()

add_executable(lcbping ${lcbsrc})
target_link_libraries(lcbping ${targets} Threads::Threads)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(lcbping PRIVATE
        -Wall -Wstrict-prototypes -Wmissing-prototypes -Wpointer-arith)
endif()

#
# "make install" rule
#
install(TARGETS lcbping RUNTIME DESTINATION bin)

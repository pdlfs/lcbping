#
# Findpsm.cmake  find psm and setup an imported target for it
#

#
# input:
#   PSM_INCLUDE_DIR: hint for finding psm.h
#   PSM_LIBRARY_DIR: hint for finding libpsm_infinipath
#
# output:
#   "psm" library target
#   PSM_FOUND (set if found)
#

include(FindPackageHandleStandardArgs)

find_path(PSM_INCLUDE psm.h HINTS ${PSM_INCLUDE_DIR})
find_library(PSM_LIBRARY psm_infinipath HINTS ${PSM_LIBRARY_DIR})

find_package_handle_standard_args(PSM DEFAULT_MSG PSM_INCLUDE PSM_LIBRARY)
mark_as_advanced (PSM_INCLUDE PSM_LIBRARY)

if(PSM_FOUND AND NOT TARGET psm)
    add_library(psm UNKNOWN IMPORTED)
    set_target_properties(psm PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${PSM_INCLUDE}")
    set_property (TARGET psm APPEND PROPERTY
        IMPORTED_LOCATION "${PSM_LIBRARY}")
endif()

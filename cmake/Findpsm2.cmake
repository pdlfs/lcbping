#
# Findpsm2.cmake  find psm2 and setup an imported target for it
#

#
# input:
#   PSM2_INCLUDE_DIR: hint for finding psm2.h
#   PSM2_LIBRARY_DIR: hint for finding libpsm2
#
# output:
#   "psm2" library target
#   PSM2_FOUND (set if found)
#

include(FindPackageHandleStandardArgs)

find_path(PSM2_INCLUDE psm2.h HINTS ${PSM2_INCLUDE_DIR})
find_library(PSM2_LIBRARY psm2 HINTS ${PSM2_LIBRARY_DIR})

find_package_handle_standard_args(PSM2 DEFAULT_MSG PSM2_INCLUDE PSM2_LIBRARY)
mark_as_advanced (PSM2_INCLUDE PSM2_LIBRARY)

if(PSM2_FOUND AND NOT TARGET psm2)
    add_library(psm2 UNKNOWN IMPORTED)
    set_target_properties(psm2 PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${PSM2_INCLUDE}")
    set_property (TARGET psm2 APPEND PROPERTY
        IMPORTED_LOCATION "${PSM2_LIBRARY}")
endif()

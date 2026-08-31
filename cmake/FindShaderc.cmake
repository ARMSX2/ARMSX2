# - Try to find SHADERC
# Once done this will define
#  SHADERC_FOUND - System has SHADERC
#  SHADERC_INCLUDE_DIRS - The SHADERC include directories
#  SHADERC_LIBRARIES - The libraries needed to use SHADERC

find_path(
    SHADERC_INCLUDE_DIR shaderc/shaderc.h
    ${SHADERC_PATH_INCLUDES}
)

# shaderc_combined is the static archive with glslang and SPIRV-Tools already
# folded in. It is what a cross build wants - a libretro core has to carry its
# own copy, since there is no shaderc on an Android device to link against -
# and it comes last so a system shared library still wins where there is one.
find_library(
    SHADERC_LIBRARY
    NAMES shaderc_shared.1 shaderc_shared shaderc_combined
    PATHS ${ADDITIONAL_LIBRARY_PATHS} ${SHADERC_PATH_LIB}
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Shaderc DEFAULT_MSG
                                  SHADERC_LIBRARY SHADERC_INCLUDE_DIR)

if(SHADERC_FOUND)
    add_library(Shaderc::shaderc_shared UNKNOWN IMPORTED)
    set_target_properties(Shaderc::shaderc_shared PROPERTIES
        IMPORTED_LOCATION ${SHADERC_LIBRARY}
        INTERFACE_INCLUDE_DIRECTORIES ${SHADERC_INCLUDE_DIR}
    )
    # Only the shared library is declared as one: the define picks the
    # dllimport half of shaderc's headers, which is wrong for the archive.
    if(NOT SHADERC_LIBRARY MATCHES "shaderc_combined")
        set_target_properties(Shaderc::shaderc_shared PROPERTIES
            INTERFACE_COMPILE_DEFINITIONS "SHADERC_SHAREDLIB"
        )
    endif()
endif()

mark_as_advanced(SHADERC_INCLUDE_DIR SHADERC_LIBRARY)

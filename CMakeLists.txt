# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

# Honor PROJECT_VER from the environment (set by CI from github.ref_name on tag
# builds), otherwise derive from git. Without this, `git describe` can pick a
# different tag than the one that triggered the workflow when multiple tags
# point at the same commit.
if(DEFINED ENV{PROJECT_VER} AND NOT "$ENV{PROJECT_VER}" STREQUAL "")
    set(PROJECT_VER "$ENV{PROJECT_VER}")
else()
    execute_process(
        COMMAND git describe --always --dirty --tags
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE PROJECT_VER
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(NOT PROJECT_VER)
        set(PROJECT_VER "unknown")
    endif()
endif()

# Force CMake to use our version
set(PROJECT_VER ${PROJECT_VER} CACHE STRING "Project version from git" FORCE)

# Monitor git files to trigger reconfigure when git state changes
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/.git/index
    ${CMAKE_SOURCE_DIR}/.git/HEAD
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(pool-controller)
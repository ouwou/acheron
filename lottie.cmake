# Optional rlottie discovery, normalized into an INTERFACE target named `lottie` the way
# ffmpeg.cmake does it. Like ENABLE_FFMPEG, ENABLE_LOTTIE fails configure when unmet.

if(NOT ENABLE_LOTTIE)
    return()
endif()

# 1. rlottie's CMake config package (vcpkg, or an installed upstream build).
#
#    vcpkg's static rlottie links a `RapidJSON` target its config never defines, which
#    reaches the linker as a nonexistent RapidJSON.lib unless the target exists here.
find_package(RapidJSON CONFIG QUIET)
find_package(rlottie CONFIG QUIET)
if(TARGET rlottie::rlottie)
    add_library(lottie INTERFACE)
    target_link_libraries(lottie INTERFACE rlottie::rlottie)
    message(STATUS "Using rlottie from ${rlottie_DIR}")
    return()
endif()

# 2. pkg-config (distro packages). Skipped under MSVC for the reason ffmpeg.cmake gives.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND AND NOT MSVC)
    pkg_check_modules(RLOTTIE_PC QUIET IMPORTED_TARGET rlottie)
    if(TARGET PkgConfig::RLOTTIE_PC)
        add_library(lottie INTERFACE)
        target_link_libraries(lottie INTERFACE PkgConfig::RLOTTIE_PC)
        message(STATUS "Using rlottie from pkg-config")
        return()
    endif()
endif()

message(FATAL_ERROR
    "ENABLE_LOTTIE is on but rlottie was not found. Lottie stickers need it: build with the "
    "bundled vcpkg (USE_VCPKG=ON installs it through the manifest's `lottie` feature), install "
    "your distribution's rlottie development package, or point CMAKE_PREFIX_PATH at an rlottie "
    "install prefix. Configure with -DENABLE_LOTTIE=OFF to build without Lottie stickers")

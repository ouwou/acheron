# Fork: ARM64 counterpart of x64-windows-static-md.cmake, used by build-windows-arm64.yml.
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

if(PORT STREQUAL "opus")
    set(VCPKG_LIBRARY_LINKAGE dynamic)
endif()

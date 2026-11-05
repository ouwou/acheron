# What sharing a screen and its sound needs on Linux beyond ffmpeg, normalized into a single
# INTERFACE target named `linuxcapture`. Each part is optional: without one the build offers
# that much less.
#   libpipewire-0.3  the portal's pictures, and what programs are playing
#   Qt DBus          asking the desktop portal for a screen or a window
#   xcb              listing an X11 display's screens and windows
#
# libpipewire has to be the machine's own, so a portable build cant bring one, and linked the
# ordinary way it wouldnt start without it. With Implib.so's implib-gen.py as IMPLIB_GEN it is
# loaded on first call instead, which Core/Media/PipeWireLibrary.hpp checks beforehand.

set(IMPLIB_GEN "" CACHE FILEPATH "Implib.so's implib-gen.py, to load libpipewire when it is first called and not at start")

add_library(linuxcapture INTERFACE)
if(NOT UNIX OR APPLE OR NOT ENABLE_VIDEO OR NOT ENABLE_VOICE)
    return()
endif()

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(PIPEWIRE QUIET IMPORTED_TARGET libpipewire-0.3)
    if(TARGET PkgConfig::PIPEWIRE)
        target_compile_definitions(linuxcapture INTERFACE ACHERON_HAVE_PIPEWIRE)
        target_link_libraries(linuxcapture INTERFACE ${CMAKE_DL_LIBS})
        if(IMPLIB_GEN)
            enable_language(C ASM)
            find_package(Python3 REQUIRED COMPONENTS Interpreter)
            find_package(Threads REQUIRED)
            set(_pipewire_library ${PIPEWIRE_LINK_LIBRARIES})
            list(FILTER _pipewire_library INCLUDE REGEX "pipewire")
            if(NOT _pipewire_library)
                message(FATAL_ERROR "IMPLIB_GEN is set, but pkg-config names no libpipewire to make stubs of")
            endif()
            list(GET _pipewire_library 0 _pipewire_library)
            get_filename_component(_pipewire_name "${_pipewire_library}" NAME)
            set(_pipewire_stubs "${CMAKE_CURRENT_BINARY_DIR}/pipewire-stubs")
            add_custom_command(
                OUTPUT "${_pipewire_stubs}/${_pipewire_name}.tramp.S" "${_pipewire_stubs}/${_pipewire_name}.init.c"
                COMMAND Python3::Interpreter "${IMPLIB_GEN}" --quiet --outdir "${_pipewire_stubs}" "${_pipewire_library}"
                DEPENDS "${IMPLIB_GEN}"
                VERBATIM)
            add_library(pipewire_when_called STATIC "${_pipewire_stubs}/${_pipewire_name}.tramp.S" "${_pipewire_stubs}/${_pipewire_name}.init.c")
            set_target_properties(pipewire_when_called PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF POSITION_INDEPENDENT_CODE ON)
            target_link_libraries(pipewire_when_called PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
            target_include_directories(linuxcapture INTERFACE ${PIPEWIRE_INCLUDE_DIRS})
            target_compile_options(linuxcapture INTERFACE ${PIPEWIRE_CFLAGS_OTHER})
            target_link_libraries(linuxcapture INTERFACE pipewire_when_called)
        else()
            target_link_libraries(linuxcapture INTERFACE PkgConfig::PIPEWIRE)
        endif()
    endif()

    pkg_check_modules(XCB QUIET IMPORTED_TARGET xcb xcb-randr xcb-composite)
    if(TARGET PkgConfig::XCB)
        target_link_libraries(linuxcapture INTERFACE PkgConfig::XCB)
        target_compile_definitions(linuxcapture INTERFACE ACHERON_HAVE_XCB)
    endif()
endif()

find_package(Qt${QT_VERSION_MAJOR} QUIET COMPONENTS DBus)
if(TARGET Qt::DBus)
    target_link_libraries(linuxcapture INTERFACE Qt::DBus)
    target_compile_definitions(linuxcapture INTERFACE ACHERON_HAVE_DBUS)
endif()

message(STATUS "Linux capture: pipewire=${PIPEWIRE_FOUND} xcb=${XCB_FOUND} dbus=${Qt${QT_VERSION_MAJOR}DBus_FOUND}")

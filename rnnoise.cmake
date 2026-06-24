if(ENABLE_VOICE AND ENABLE_RNNOISE)
    find_package(rnnoise QUIET)
    if(NOT TARGET rnnoise)
        foreach(_rnnoise_target rnnoise::rnnoise RNNoise::rnnoise)
            if(TARGET ${_rnnoise_target})
                add_library(rnnoise INTERFACE)
                target_link_libraries(rnnoise INTERFACE ${_rnnoise_target})
                break()
            endif()
        endforeach()
    endif()
    if(TARGET rnnoise)
        return()
    endif()
    if(rnnoise_FOUND)
        message(STATUS "rnnoise package found without usable target and will be included as a submodule")
    else()
        message(STATUS "rnnoise was not found and will be included as a submodule")
    endif()

    enable_language(C)

    set(RNNOISE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/rnnoise")
    set(RNNOISE_MODEL_DIR "${CMAKE_BINARY_DIR}/rnnoise-generated")

    if(NOT EXISTS "${RNNOISE_DIR}/src/denoise.c")
        message(FATAL_ERROR "vendor/rnnoise is empty")
    endif()

    if(NOT EXISTS "${RNNOISE_MODEL_DIR}/rnnoise_data.c")
        set(RNNOISE_MODEL_URL "https://media.xiph.org/rnnoise/models/rnnoise_data-0a8755f8e2d834eff6a54714ecc7d75f9932e845df35f8b59bc52a7cfe6e8b37.tar.gz")
        set(RNNOISE_MODEL_SHA512 "b327d2fc5095be9ed66c5246a86b1a1ce180e9de875c4e5e8778f975560d1f035da40a8686dc1c3fd91c8e709be65d2638eccaa9f866b6f3d85f8d0d16bd2184")
        set(RNNOISE_MODEL_TARBALL "${CMAKE_BINARY_DIR}/rnnoise_data.tar.gz")
        set(RNNOISE_MODEL_EXTRACT "${CMAKE_BINARY_DIR}/rnnoise-model-extract")

        message(STATUS "Downloading RNNoise model data...")
        file(DOWNLOAD "${RNNOISE_MODEL_URL}" "${RNNOISE_MODEL_TARBALL}"
            EXPECTED_HASH SHA512=${RNNOISE_MODEL_SHA512}
            STATUS RNNOISE_DL_STATUS)
        list(GET RNNOISE_DL_STATUS 0 RNNOISE_DL_CODE)
        if(NOT RNNOISE_DL_CODE EQUAL 0)
            list(GET RNNOISE_DL_STATUS 1 RNNOISE_DL_MSG)
            message(FATAL_ERROR "Failed to download RNNoise model data: ${RNNOISE_DL_MSG}. "
                "Configure with -DENABLE_RNNOISE=OFF to build without noise suppression.")
        endif()

        file(REMOVE_RECURSE "${RNNOISE_MODEL_EXTRACT}")
        file(MAKE_DIRECTORY "${RNNOISE_MODEL_EXTRACT}")
        execute_process(
            COMMAND ${CMAKE_COMMAND} -E tar xzf "${RNNOISE_MODEL_TARBALL}"
            WORKING_DIRECTORY "${RNNOISE_MODEL_EXTRACT}"
            RESULT_VARIABLE RNNOISE_EXTRACT_RESULT)
        if(NOT RNNOISE_EXTRACT_RESULT EQUAL 0)
            message(FATAL_ERROR "Failed to extract RNNoise model data")
        endif()

        file(GLOB_RECURSE RNNOISE_MODEL_FILES
            "${RNNOISE_MODEL_EXTRACT}/*rnnoise_data.c"
            "${RNNOISE_MODEL_EXTRACT}/*rnnoise_data.h")
        if(NOT RNNOISE_MODEL_FILES)
            message(FATAL_ERROR "RNNoise model data not found after extraction")
        endif()
        file(MAKE_DIRECTORY "${RNNOISE_MODEL_DIR}")
        file(COPY ${RNNOISE_MODEL_FILES} DESTINATION "${RNNOISE_MODEL_DIR}")
    endif()

    set(RNNOISE_SOURCES
        ${RNNOISE_DIR}/src/denoise.c
        ${RNNOISE_DIR}/src/rnn.c
        ${RNNOISE_DIR}/src/pitch.c
        ${RNNOISE_DIR}/src/kiss_fft.c
        ${RNNOISE_DIR}/src/celt_lpc.c
        ${RNNOISE_DIR}/src/nnet.c
        ${RNNOISE_DIR}/src/nnet_default.c
        ${RNNOISE_DIR}/src/parse_lpcnet_weights.c
        ${RNNOISE_DIR}/src/rnnoise_tables.c
        ${RNNOISE_MODEL_DIR}/rnnoise_data.c
    )

    set(RNNOISE_X86 FALSE)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|x64|i[3-6]86|x86|X86)$")
        set(RNNOISE_X86 TRUE)
    endif()

    set(RNNOISE_USE_RTCD FALSE)
    if(RNNOISE_X86 AND ENABLE_RNNOISE_RTCD)
        set(RNNOISE_USE_RTCD TRUE)
        list(APPEND RNNOISE_SOURCES
            ${RNNOISE_DIR}/src/x86/x86cpu.c
            ${RNNOISE_DIR}/src/x86/x86_dnn_map.c
            ${RNNOISE_DIR}/src/x86/nnet_sse4_1.c
            ${RNNOISE_DIR}/src/x86/nnet_avx2.c
        )
    endif()

    add_library(rnnoise STATIC ${RNNOISE_SOURCES})
    target_include_directories(rnnoise
        PUBLIC ${RNNOISE_DIR}/include
        PRIVATE ${RNNOISE_DIR}/src ${RNNOISE_MODEL_DIR})

    set_target_properties(rnnoise PROPERTIES POSITION_INDEPENDENT_CODE ON)
    if(MSVC)
        target_compile_definitions(rnnoise PRIVATE _CRT_SECURE_NO_WARNINGS restrict=__restrict)
    endif()
    if(NOT WIN32)
        target_link_libraries(rnnoise PRIVATE m)
    endif()

    if(RNNOISE_USE_RTCD)
        target_compile_definitions(rnnoise PRIVATE RNN_ENABLE_X86_RTCD)
        if(MSVC)
            set_source_files_properties(${RNNOISE_DIR}/src/x86/nnet_sse4_1.c PROPERTIES
                COMPILE_DEFINITIONS "OPUS_X86_MAY_HAVE_SSE2;OPUS_X86_MAY_HAVE_SSE4_1")
            set_source_files_properties(${RNNOISE_DIR}/src/x86/nnet_avx2.c PROPERTIES
                COMPILE_OPTIONS "/arch:AVX2"
                COMPILE_DEFINITIONS "OPUS_X86_MAY_HAVE_SSE2;OPUS_X86_MAY_HAVE_SSE4_1")
        else()
            target_compile_definitions(rnnoise PRIVATE CPU_INFO_BY_C)
            set_source_files_properties(${RNNOISE_DIR}/src/x86/nnet_sse4_1.c
                PROPERTIES COMPILE_OPTIONS "-msse4.1")
            set_source_files_properties(${RNNOISE_DIR}/src/x86/nnet_avx2.c
                PROPERTIES COMPILE_OPTIONS "-mavx2;-mfma")
        endif()
    endif()
endif()

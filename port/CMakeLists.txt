# lost - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(lost LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Built for x86-64-v3 (AVX2 + FMA, passed by tools/build.ps1) so the translated PowerPC fused
# multiply-adds become hardware FMA instead of CRT calls. No FP contraction: clang would otherwise
# fuse a separate a*b+c into one FMA, rounding once where the PowerPC rounded twice (as GRAW).
add_compile_options($<$<CXX_COMPILER_ID:Clang,AppleClang>:-ffp-contract=off>)

include(generated/rexglue.cmake)

# Sources
set(LOST_SOURCES
    src/main.cpp
    src/crash_report.cpp
    src/bink_probe.cpp
    src/perf.cpp
    src/thread_records.cpp
    src/settings.cpp
    src/autopilot.cpp
    src/ui_probe.cpp
    src/native_options.cpp
)

if(WIN32)
    add_executable(lost WIN32 ${LOST_SOURCES})
else()
    add_executable(lost ${LOST_SOURCES})
endif()

rexglue_setup_target(lost GPU_PLUGINS xenos)
if(WIN32)
    target_link_libraries(lost PRIVATE dbghelp)
endif()

# The native renderer (native/, docs/NATIVE_RENDERER.md), off at run time unless --lost_native.
# D3D12 only, hence Windows only: the original tree's Vulkan backend (vulkan_*.cpp/h,
# spirv_translate.cpp, sdk_linux_shim.cpp) is not vendored. It compiles ReXGlue's shader
# translators and texture format tables from the SDK source tree (the plugin exports none of
# them): third_party/rexglue-sdk-bd833a2, the same commit as the SDK the game links.
if(WIN32)
    set(LOST_REXGLUE_SDK_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/rexglue-sdk-bd833a2"
        CACHE PATH "ReXGlue SDK source tree (bd833a2) for the native renderer")
    if(EXISTS "${LOST_REXGLUE_SDK_SRC}/src/graphics/pipeline/shader/dxbc_translator.cpp")
        # The tree must carry the SDK patches the native renderer compiles against (tools/setup.ps1
        # applies them): a line each one adds, so an unpatched tree stops here rather than in
        # native/'s compile or link errors.
        foreach(check
                "pgr4-native/0021-*.patch|include/rex/graphics/format/dxbc.h|kSampleB = 74"
                "pgr4-native/0022-*.patch|include/rex/graphics/pipeline/shader/dxbc_translator.h|textures_unsigned"
                "rexglue-bd833a2-gpu-skip-draws.patch|include/rex/graphics/command_processor.h|for presenters"
                "rexglue-bd833a2-gpu-skip-draws.patch|src/graphics/command_processor.cpp|gpu_skip_draws"
                "rexglue-bd833a2-implicit-gradients-switch.patch|src/graphics/pipeline/shader/dxbc_translator_fetch.cpp|bool g_dxbc_implicit_gradients"
                "rexglue-bd833a2-spirv-exp-adjust.patch|src/graphics/pipeline/shader/spirv_translator_fetch.cpp|fetch_constant_word_3_signed"
                "rexglue-bd833a2-spirv-implicit-gradients.patch|src/graphics/pipeline/shader/spirv_translator_fetch.cpp|bool g_spirv_implicit_gradients")
            string(REPLACE "|" ";" check "${check}")
            list(GET check 0 patch)
            list(GET check 1 file)
            list(GET check 2 marker)
            file(READ "${LOST_REXGLUE_SDK_SRC}/${file}" text)
            string(FIND "${text}" "${marker}" at)
            if(at EQUAL -1)
                message(FATAL_ERROR "The native renderer needs the SDK patch tools/patches/${patch} in "
                    "${LOST_REXGLUE_SDK_SRC} (${file} lacks it): run tools/setup.ps1 -Force "
                    "(docs/SETUP.md), or remove the tree to build without the native renderer.")
            endif()
        endforeach()
        set(LOST_NATIVE_SRC "${CMAKE_CURRENT_SOURCE_DIR}/native")
        file(GLOB LOST_NATIVE_SOURCES CONFIGURE_DEPENDS "${LOST_NATIVE_SRC}/*.cpp")
        # An object library: its hooks (REX_HOOK_RAW) override the generated functions only when
        # their objects are linked in, and it compiles against the SDK source tree's headers first,
        # which carry the pgr4-native SDK patches the renderer needs (tools/patches/pgr4-native/:
        # shader translator modifications, the command processor's gamma ramp, gpu_skip_draws).
        add_library(lost_native OBJECT ${LOST_NATIVE_SOURCES}
            "${LOST_REXGLUE_SDK_SRC}/src/graphics/pipeline/texture/util.cpp"
            "${LOST_REXGLUE_SDK_SRC}/src/graphics/pipeline/texture/info_formats.cpp")
        target_include_directories(lost_native BEFORE PRIVATE "${LOST_REXGLUE_SDK_SRC}/include")
        target_include_directories(lost_native PRIVATE "${LOST_NATIVE_SRC}" "${LOST_REXGLUE_SDK_SRC}")
        rexglue_apply_target_settings(lost_native)
        target_link_libraries(lost_native PRIVATE rex::runtime rex::xxhash)
        include("${LOST_NATIVE_SRC}/lost_shader_xlate.cmake")
        target_include_directories(lost_shader_xlate BEFORE PRIVATE "${LOST_REXGLUE_SDK_SRC}/include")
        target_link_libraries(lost PRIVATE lost_native lost_shader_xlate)
        target_link_libraries(lost PRIVATE d3d12 dxgi dxguid)
        target_compile_definitions(lost PRIVATE LOST_NATIVE_RENDERER=1)
        # The recorded pipelines (tools/pipeline_manifest.py: hashes and pipeline state, no shader
        # code), beside lost.exe so the renderer builds them at start instead of hitching in play.
        add_custom_target(lost_pipeline_manifest ALL
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${CMAKE_CURRENT_SOURCE_DIR}/native/pipelines.manifest" "$<TARGET_FILE_DIR:lost>/pipelines.manifest"
            VERBATIM)
    else()
        message(STATUS "lost: native renderer not built (needs ${LOST_REXGLUE_SDK_SRC})")
    endif()
endif()

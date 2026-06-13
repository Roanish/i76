cmake_minimum_required(VERSION 3.16)
project(i76 VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
)

# -------------------------------------------------------------------------
# Dependencies
# -------------------------------------------------------------------------

find_package(SDL2   REQUIRED)
find_package(Vulkan REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LZO2 REQUIRED lzo2)

# glslc — GLSL to SPIR-V compiler (ships with the Vulkan SDK)
find_program(GLSLC glslc HINTS ENV VULKAN_SDK PATH_SUFFIXES bin
    DOC "GLSL to SPIR-V compiler")
if(NOT GLSLC)
    message(FATAL_ERROR "glslc not found. Install the Vulkan SDK or set VULKAN_SDK.")
endif()

# -------------------------------------------------------------------------
# Shader compilation → embedded C headers
# -------------------------------------------------------------------------

set(SHADER_SRC_DIR  ${CMAKE_SOURCE_DIR}/src/render/shaders)
set(SHADER_GEN_DIR  ${CMAKE_BINARY_DIR}/shaders_gen)
file(MAKE_DIRECTORY ${SHADER_GEN_DIR})

function(embed_shader TARGET STAGE_EXT VAR_NAME)
    set(glsl_src  ${SHADER_SRC_DIR}/blit.${STAGE_EXT})
    set(spv_out   ${SHADER_GEN_DIR}/blit_${STAGE_EXT}.spv)
    set(hdr_out   ${SHADER_GEN_DIR}/blit_${STAGE_EXT}_spv.h)

    add_custom_command(
        OUTPUT  ${spv_out}
        COMMAND ${GLSLC} -o ${spv_out} ${glsl_src}
        DEPENDS ${glsl_src}
        COMMENT "glslc: blit.${STAGE_EXT}"
    )
    add_custom_command(
        OUTPUT  ${hdr_out}
        COMMAND ${CMAKE_COMMAND}
                    -DINPUT=${spv_out}
                    -DOUTPUT=${hdr_out}
                    -DVAR=${VAR_NAME}
                    -P ${CMAKE_SOURCE_DIR}/cmake/embed_spv.cmake
        DEPENDS ${spv_out}
        COMMENT "embed: blit_${STAGE_EXT}_spv.h"
    )
    target_sources(${TARGET} PRIVATE ${hdr_out})
endfunction()

# -------------------------------------------------------------------------
# Sources
# -------------------------------------------------------------------------

file(GLOB_RECURSE SOURCES src/*.c)

# libsmacker — Smacker video decoder (only the three core C files; skip UnityInterface.cpp)
set(LIBSMACKER_SOURCES
    lib/libsmacker/smacker.c
    lib/libsmacker/smk_bitstream.c
    lib/libsmacker/smk_hufftree.c
)
set_source_files_properties(${LIBSMACKER_SOURCES} PROPERTIES COMPILE_FLAGS "-Wno-pedantic")

add_executable(i76 ${SOURCES} ${LIBSMACKER_SOURCES})

embed_shader(i76 vert blit_vert_spv)
embed_shader(i76 frag blit_frag_spv)

target_include_directories(i76 PRIVATE
    src/
    lib/                     # so video.c can #include "libsmacker/smacker.h"
    ${SDL2_INCLUDE_DIRS}
    ${Vulkan_INCLUDE_DIRS}
    ${SHADER_GEN_DIR}        # so render_vk.c can #include "blit_vert_spv.h"
)

target_link_libraries(i76 PRIVATE
    ${SDL2_LIBRARIES}
    ${Vulkan_LIBRARIES}
    ${LZO2_LIBRARIES}
    m
)

target_include_directories(i76 PRIVATE ${LZO2_INCLUDE_DIRS})

# Debug build: enable Vulkan validation layers
target_compile_definitions(i76 PRIVATE
    $<$<CONFIG:Debug>:I76_VULKAN_VALIDATION>
)

# Binary lands in project root (same as before)
set_target_properties(i76 PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}
)

# -------------------------------------------------------------------------
# Tools
# -------------------------------------------------------------------------

add_executable(smk_dump tools/smk_dump.c ${LIBSMACKER_SOURCES})

# zfs_dump shares the engine's ZFS reader so it decompresses exactly like the game.
add_executable(zfs_dump tools/zfs_dump.c src/engine/zfs.c)
target_include_directories(zfs_dump PRIVATE src/ ${LZO2_INCLUDE_DIRS})
target_link_libraries(zfs_dump PRIVATE ${LZO2_LIBRARIES})

set(BROWSER_ENGINE_SOURCES
    src/engine/fs.c
    src/engine/zfs.c
    src/engine/vfs.c
    src/engine/pcx.c
)
pkg_check_modules(SDL2_TTF REQUIRED SDL2_ttf)

add_executable(asset_browser tools/asset_browser.c ${BROWSER_ENGINE_SOURCES})
target_include_directories(asset_browser PRIVATE
    src/
    ${SDL2_INCLUDE_DIRS}
    ${SDL2_TTF_INCLUDE_DIRS}
    ${LZO2_INCLUDE_DIRS}
)
target_link_libraries(asset_browser PRIVATE
    ${SDL2_LIBRARIES}
    ${SDL2_TTF_LIBRARIES}
    ${LZO2_LIBRARIES}
)
set_target_properties(asset_browser PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/tools)

set_target_properties(zfs_dump PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/tools)
target_include_directories(smk_dump PRIVATE lib/)
target_compile_options(smk_dump PRIVATE -Wno-unused-result)
set_target_properties(smk_dump PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/tools)

message(STATUS "Build type:      ${CMAKE_BUILD_TYPE}")
message(STATUS "SDL2 include:    ${SDL2_INCLUDE_DIRS}")
message(STATUS "Vulkan include:  ${Vulkan_INCLUDE_DIRS}")
message(STATUS "glslc:           ${GLSLC}")

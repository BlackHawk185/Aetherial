cmake_minimum_required(VERSION 3.15)
project(MMORPGEngineModular LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Fix runtime library mismatch - use static runtime for consistency
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Add engine source
add_subdirectory(engine)

# Create main executable
add_executable(MMORPGEngine
    main.cpp
)
target_link_libraries(MMORPGEngine PRIVATE engine)

# Copy shaders to build directory
file(GLOB SHADER_FILES "${CMAKE_SOURCE_DIR}/shaders/*.vert" "${CMAKE_SOURCE_DIR}/shaders/*.frag" "${CMAKE_SOURCE_DIR}/shaders/*.comp")
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/bin/shaders)
foreach(SHADER ${SHADER_FILES})
    get_filename_component(SHADER_NAME ${SHADER} NAME)
    configure_file(${SHADER} ${CMAKE_BINARY_DIR}/bin/shaders/${SHADER_NAME} COPYONLY)
endforeach()

# Compile Vulkan shaders to SPIR-V (REQUIRED - build fails if missing)
find_program(GLSLC glslc HINTS "$ENV{VULKAN_SDK}/Bin" "C:/VulkanSDK/1.4.328.1/Bin")
if(NOT GLSLC)
    message(FATAL_ERROR "glslc not found! Install Vulkan SDK or set VULKAN_SDK environment variable.")
endif()

file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/bin/shaders/vulkan)

# Define required shaders explicitly to catch missing files at build time
set(REQUIRED_VULKAN_SHADERS
    quad_vertex_pulling.vert
    quad.frag
    quad_gbuffer.frag
    quad_simple.frag
    lighting.vert
    lighting.frag
    lighting_pass.vert
    lighting_pass.frag
    sky.vert
    sky.frag
    highlight.vert
    highlight.frag
    clouds.vert
    clouds.frag
    ssr.comp
    sspr.comp
    model_instance.vert
    model_instance.frag
    model_forward.vert
    model_forward.frag
    composite.vert
    composite.frag
)

# Verify all required shaders exist in source
foreach(SHADER_NAME ${REQUIRED_VULKAN_SHADERS})
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/shaders/vulkan/${SHADER_NAME}")
        message(FATAL_ERROR "Required shader missing: shaders/vulkan/${SHADER_NAME}")
    endif()
endforeach()

# Compile each shader
foreach(SHADER_NAME ${REQUIRED_VULKAN_SHADERS})
    set(SHADER_SOURCE "${CMAKE_SOURCE_DIR}/shaders/vulkan/${SHADER_NAME}")
    set(SHADER_OUTPUT "${CMAKE_BINARY_DIR}/bin/shaders/vulkan/${SHADER_NAME}.spv")
    
    add_custom_command(
        OUTPUT ${SHADER_OUTPUT}
        COMMAND ${GLSLC} ${SHADER_SOURCE} -o ${SHADER_OUTPUT}
        DEPENDS ${SHADER_SOURCE}
        COMMENT "Compiling Vulkan shader: ${SHADER_NAME}"
        VERBATIM
    )
    
    list(APPEND VULKAN_SHADER_OUTPUTS ${SHADER_OUTPUT})
endforeach()

# Generate shader paths header for compile-time validation
set(SHADER_PATHS_HEADER "${CMAKE_BINARY_DIR}/engine/ShaderPaths.h")
file(WRITE ${SHADER_PATHS_HEADER} "// Auto-generated shader paths - DO NOT EDIT\n")
file(APPEND ${SHADER_PATHS_HEADER} "#pragma once\n\n")
file(APPEND ${SHADER_PATHS_HEADER} "namespace ShaderPaths {\n")
foreach(SHADER_NAME ${REQUIRED_VULKAN_SHADERS})
    string(REPLACE "." "_" SHADER_VAR ${SHADER_NAME})
    file(APPEND ${SHADER_PATHS_HEADER} "    constexpr const char* ${SHADER_VAR}_spv = \"build/bin/shaders/vulkan/${SHADER_NAME}.spv\";\n")
endforeach()
file(APPEND ${SHADER_PATHS_HEADER} "}\n")

add_custom_target(VulkanShaders ALL DEPENDS ${VULKAN_SHADER_OUTPUTS})
add_dependencies(engine VulkanShaders)  # Engine must wait for shaders
add_dependencies(MMORPGEngine VulkanShaders)

# Remove old shader files that were renamed
file(REMOVE ${CMAKE_BINARY_DIR}/bin/shaders/greedy_mesh.comp)

# Copy assets to build directory
file(GLOB_RECURSE ASSET_FILES "${CMAKE_SOURCE_DIR}/assets/*")
foreach(ASSET ${ASSET_FILES})
    file(RELATIVE_PATH REL_PATH ${CMAKE_SOURCE_DIR}/assets ${ASSET})
    get_filename_component(ASSET_DIR ${REL_PATH} DIRECTORY)
    file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/bin/assets/${ASSET_DIR})
    configure_file(${ASSET} ${CMAKE_BINARY_DIR}/bin/assets/${REL_PATH} COPYONLY)
endforeach()

# Third-party libraries
add_subdirectory(libs)
# Some libraries like imgui are not standard CMake projects, so link manually in engine/CMakeLists.txt.
# Manual linking: see engine/CMakeLists.txt for instructions.

# Link libraries in engine/CMakeLists.txt as needed

# Platform-specific settings
if(MSVC)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
endif()

# TODO: Add options for future console support

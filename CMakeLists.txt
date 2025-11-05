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
file(GLOB SHADER_FILES "${CMAKE_SOURCE_DIR}/shaders/*")
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/bin/shaders)
foreach(SHADER ${SHADER_FILES})
    get_filename_component(SHADER_NAME ${SHADER} NAME)
    configure_file(${SHADER} ${CMAKE_BINARY_DIR}/bin/shaders/${SHADER_NAME} COPYONLY)
endforeach()

# Remove old shader files that were renamed
file(REMOVE ${CMAKE_BINARY_DIR}/bin/shaders/greedy_mesh.comp)

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

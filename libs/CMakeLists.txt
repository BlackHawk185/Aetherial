# GLFW
add_subdirectory(glfw)

# ENet networking - Manual build instead of subdirectory to avoid CMake version conflicts
add_library(enet STATIC
    enet/callbacks.c
    enet/compress.c
    enet/host.c
    enet/list.c
    enet/packet.c
    enet/peer.c
    enet/protocol.c
    enet/win32.c
)

# Set up ENet target properties
target_include_directories(enet PUBLIC enet/include)
if(WIN32)
    target_link_libraries(enet PUBLIC winmm ws2_32)
endif()

# LZ4 compression library
add_library(lz4 STATIC
    lz4/lz4_simple.c
)

# Set up LZ4 target properties
target_include_directories(lz4 PUBLIC lz4)

# STB image loading library
add_library(stb STATIC
    stb/stb_image_impl.cpp
)

# Set up STB target properties
target_include_directories(stb PUBLIC stb)

# This target will be linked by the engine

# glad2: OpenGL 4.6 Core loader
include(FetchContent)
FetchContent_Declare(
    glad2
    GIT_REPOSITORY https://github.com/Dav1dde/glad.git
    GIT_TAG v2.0.6
)
FetchContent_MakeAvailable(glad2)
include(${glad2_SOURCE_DIR}/cmake/GladConfig.cmake)
glad_add_library(glad REPRODUCIBLE LANGUAGE C API gl:core=4.6 EXTENSIONS GL_KHR_debug)

# Header-only GLM (math library)
include(FetchContent)
FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG 1.0.1
)
FetchContent_MakeAvailable(glm)

# FastNoiseLite is header-only, no build needed (included directly in engine code)

# FastNoiseSIMD - SIMD-accelerated noise library
add_library(FastNoiseSIMD STATIC
    FastNoiseSIMD/FastNoiseSIMD.cpp
    FastNoiseSIMD/FastNoiseSIMD_internal.cpp
    FastNoiseSIMD/FastNoiseSIMD_sse2.cpp
    FastNoiseSIMD/FastNoiseSIMD_sse41.cpp
    FastNoiseSIMD/FastNoiseSIMD_avx2.cpp
)
target_include_directories(FastNoiseSIMD PUBLIC FastNoiseSIMD)

# Set architecture-specific flags for SIMD files
if(MSVC)
    set_source_files_properties(FastNoiseSIMD/FastNoiseSIMD_sse2.cpp PROPERTIES COMPILE_FLAGS "/arch:SSE2")
    set_source_files_properties(FastNoiseSIMD/FastNoiseSIMD_sse41.cpp PROPERTIES COMPILE_FLAGS "/arch:SSE2")
    set_source_files_properties(FastNoiseSIMD/FastNoiseSIMD_avx2.cpp PROPERTIES COMPILE_FLAGS "/arch:AVX2")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set_source_files_properties(FastNoiseSIMD/FastNoiseSIMD_sse2.cpp PROPERTIES COMPILE_FLAGS "-msse2")
    set_source_files_properties(FastNoiseSIMD/FastNoiseSIMD_sse41.cpp PROPERTIES COMPILE_FLAGS "-msse4.1")
    set_source_files_properties(FastNoiseSIMD/FastNoiseSIMD_avx2.cpp PROPERTIES COMPILE_FLAGS "-mavx2 -mfma")
endif()

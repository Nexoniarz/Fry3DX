cmake_minimum_required(VERSION 3.25)
project(Fry3DX VERSION 0.7.0 LANGUAGES C CXX)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF) # (no C++ modules; the scan can pick up a different clang than the NDK's)
if(UNIX AND NOT APPLE)
  add_link_options(-rdynamic) # function names in crash reports
endif()
# FRY_PORTABLE: a Linux build to hand out (runs on any distro as new as the one it's built on):
# SDL and the C++ library inside the program.
option(FRY_PORTABLE "Build SDL in and link the C++ runtime statically" OFF)
if(FRY_PORTABLE AND UNIX AND NOT APPLE AND NOT ANDROID)
  add_link_options(-static-libstdc++ -static-libgcc)
endif()

# Fry3DX — Frying 3D Extreme: a 3D modelling app that's easy to pick up. Its engine (engine/):
# window, renderer and UI.

find_program(FRY_CCACHE ccache)
if(FRY_CCACHE AND NOT CMAKE_CXX_COMPILER_LAUNCHER)
  set(CMAKE_C_COMPILER_LAUNCHER ${CMAKE_COMMAND} -E env CCACHE_SLOPPINESS=pch_defines,time_macros,include_file_mtime,include_file_ctime ${FRY_CCACHE})
  set(CMAKE_CXX_COMPILER_LAUNCHER ${CMAKE_C_COMPILER_LAUNCHER})
endif()
find_program(FRY_MOLD mold)
if(FRY_MOLD AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_link_options(-fuse-ld=mold)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Where the content goes: next to the program; on a Mac, inside the app (Fry3DX.app/Contents/
# Resources, where SDL_GetBasePath points).
if(APPLE)
  set(FRY_CONTENT_DIR ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Fry3DX.app/Contents/Resources)
else()
  set(FRY_CONTENT_DIR ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
endif()

# Libraries are linked in, not .so files beside the program (SDL says for itself). Set before
# any of them, so one that has its own BUILD_SHARED_LIBS option (Manifold) can't turn it on.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build the libraries as shared ones" FORCE)
include(cmake/Dependencies.cmake)
include(cmake/Shaders.cmake)

add_subdirectory(engine)
add_subdirectory(src)

# Content next to the program: engine/ (the engine's: fonts, sky, config); Fry3DX/ (the app's own: scripts, branding).
add_custom_target(content ALL
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/content ${FRY_CONTENT_DIR}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${FRY_CONTENT_DIR}/engine/fonts ${FRY_CONTENT_DIR}/Fry3DX
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${dejavu_SOURCE_DIR}/ttf/DejaVuSans.ttf ${dejavu_SOURCE_DIR}/ttf/DejaVuSans-Bold.ttf
          ${dejavu_SOURCE_DIR}/LICENSE ${FRY_CONTENT_DIR}/engine/fonts/
  COMMENT "Copying content" VERBATIM)

enable_testing()
add_subdirectory(tests EXCLUDE_FROM_ALL)
add_custom_target(check COMMAND ${CMAKE_CTEST_COMMAND} --test-dir ${CMAKE_BINARY_DIR} --output-on-failure
                  DEPENDS mesh_test script_test USES_TERMINAL VERBATIM)

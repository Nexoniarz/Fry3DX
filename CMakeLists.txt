cmake_minimum_required(VERSION 3.25)
project(Fry3DX VERSION 0.5.0 LANGUAGES C CXX)
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

include(cmake/Dependencies.cmake)
include(cmake/Shaders.cmake)

add_subdirectory(engine)
add_subdirectory(src)

# Content next to the program: bin/engine/ (the engine's: fonts, sky, config); bin/Fry3DX/ (the app's own, none yet).
add_custom_target(content ALL
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/content ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/engine/fonts ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Fry3DX
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${dejavu_SOURCE_DIR}/ttf/DejaVuSans.ttf ${dejavu_SOURCE_DIR}/ttf/DejaVuSans-Bold.ttf
          ${dejavu_SOURCE_DIR}/LICENSE ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/engine/fonts/
  COMMENT "Copying content" VERBATIM)

enable_testing()
add_subdirectory(tests EXCLUDE_FROM_ALL)
add_custom_target(check COMMAND ${CMAKE_CTEST_COMMAND} --test-dir ${CMAKE_BINARY_DIR} --output-on-failure
                  DEPENDS mesh_test USES_TERMINAL VERBATIM)

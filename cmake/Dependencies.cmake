# Third-party code, pinned. Everything is fetched so Windows and Linux build the same way.
include(FetchContent)
set(FETCHCONTENT_QUIET ON)

set(KEEL_VK_SDK 1.4.363)

FetchContent_Declare(VulkanHeaders GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
                     GIT_TAG vulkan-sdk-${KEEL_VK_SDK}.0 GIT_SHALLOW ON SYSTEM)
FetchContent_Declare(volk GIT_REPOSITORY https://github.com/zeux/volk.git
                     GIT_TAG vulkan-sdk-${KEEL_VK_SDK}.0 GIT_SHALLOW ON SYSTEM)
FetchContent_Declare(vkbootstrap GIT_REPOSITORY https://github.com/charles-lunarg/vk-bootstrap.git
                     GIT_TAG v${KEEL_VK_SDK} GIT_SHALLOW ON SYSTEM)
FetchContent_Declare(VulkanMemoryAllocator GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
                     GIT_TAG v3.3.0 GIT_SHALLOW ON SYSTEM)
# Uses an installed SDL3 if there is one (Nix shell), otherwise builds it.
FetchContent_Declare(SDL3 GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
                     GIT_TAG release-3.4.18 GIT_SHALLOW ON SYSTEM FIND_PACKAGE_ARGS CONFIG)
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git
                     GIT_TAG v1.92.9b-docking GIT_SHALLOW ON SYSTEM)
FetchContent_Declare(glm GIT_REPOSITORY https://github.com/g-truc/glm.git
                     GIT_TAG 1.0.3 GIT_SHALLOW ON SYSTEM)
FetchContent_Declare(zstd GIT_REPOSITORY https://github.com/facebook/zstd.git
                     GIT_TAG v1.5.7 GIT_SHALLOW ON SYSTEM SOURCE_SUBDIR build/cmake)
FetchContent_Declare(cgltf GIT_REPOSITORY https://github.com/jkuhlmann/cgltf.git
                     GIT_TAG v1.15 GIT_SHALLOW ON SYSTEM SOURCE_SUBDIR no-cmake)
FetchContent_Declare(stb GIT_REPOSITORY https://github.com/nothings/stb.git
                     GIT_TAG 2c980bb59875b0d32144a71867fbdebb2f77cd20 SYSTEM SOURCE_SUBDIR no-cmake)
FetchContent_Declare(miniaudio GIT_REPOSITORY https://github.com/mackron/miniaudio.git
                     GIT_TAG 0.11.25 GIT_SHALLOW ON SYSTEM SOURCE_SUBDIR no-cmake)
FetchContent_Declare(ogg GIT_REPOSITORY https://github.com/xiph/ogg.git
                     GIT_TAG v1.3.6 GIT_SHALLOW ON SYSTEM SOURCE_SUBDIR no-cmake)
FetchContent_Declare(opus GIT_REPOSITORY https://github.com/xiph/opus.git
                     GIT_TAG v1.6.1 GIT_SHALLOW ON SYSTEM)
FetchContent_Declare(opusfile GIT_REPOSITORY https://github.com/xiph/opusfile.git
                     GIT_TAG v0.12 GIT_SHALLOW ON SYSTEM SOURCE_SUBDIR no-cmake)
# DejaVu Sans (Bitstream Vera licence): the UI font, close to Source's Verdana look.
FetchContent_Declare(dejavu URL https://github.com/dejavu-fonts/dejavu-fonts/releases/download/version_2_37/dejavu-fonts-ttf-2.37.zip
                     URL_HASH SHA256=7576310b219e04159d35ff61dd4a4ec4cdba4f35c00e002a136f00e96a908b0a
                     DOWNLOAD_EXTRACT_TIMESTAMP ON SOURCE_SUBDIR no-cmake)
# Lua: the scripting language (Script panel, scripts/*.lua). Small, plain C, nothing else needed.
FetchContent_Declare(lua URL https://www.lua.org/ftp/lua-5.4.7.tar.gz
                     URL_HASH SHA256=9fbf5e28ef86c69858f6d3d34eccc32e911c1a28b4120ff3e84aaa70cfbf1e30
                     DOWNLOAD_EXTRACT_TIMESTAMP ON SOURCE_SUBDIR no-cmake)
# KEEL_YAPF_DIR=/path/to/YAPF uses a local checkout instead.
FetchContent_Declare(yapf GIT_REPOSITORY https://github.com/Nexoniarz/yapf.git
                     GIT_TAG 06a4e7db9e7e6d90132473e5606f0e6f5b1d6001 SYSTEM) # YAPF 2 (+ yapf_tex_to_rgba8: any surface as RGBA8)
if(KEEL_YAPF_DIR)
  set(FETCHCONTENT_SOURCE_DIR_YAPF ${KEEL_YAPF_DIR})
endif()

set(VOLK_PULL_IN_VULKAN OFF)
if(FRY_PORTABLE) # (SDL inside the program: one file to run; it opens X11, Wayland and sound itself)
  set(FETCHCONTENT_TRY_FIND_PACKAGE_MODE NEVER)
  set(SDL_SHARED OFF)
  set(SDL_STATIC ON)
else()
  set(SDL_SHARED ON)
  set(SDL_STATIC OFF)
endif()
set(GLM_BUILD_TESTS OFF)
set(ZSTD_BUILD_PROGRAMS OFF)
set(ZSTD_BUILD_SHARED OFF)
set(ZSTD_BUILD_TESTS OFF)
set(ZSTD_LEGACY_SUPPORT OFF)
if(ANDROID)
  set(VK_BOOTSTRAP_POSITION_INDEPENDENT_CODE ON CACHE BOOL "" FORCE) # (it sets its own, off unless shared)
endif()
FetchContent_MakeAvailable(VulkanHeaders volk vkbootstrap VulkanMemoryAllocator SDL3 glm zstd cgltf stb)

# zstd's seekable format (contrib): a normal .zst that also allows random access. YMP packs use it.
add_library(zstd_seekable STATIC
  ${zstd_SOURCE_DIR}/contrib/seekable_format/zstdseek_compress.c
  ${zstd_SOURCE_DIR}/contrib/seekable_format/zstdseek_decompress.c)
target_include_directories(zstd_seekable SYSTEM PUBLIC ${zstd_SOURCE_DIR}/contrib/seekable_format ${zstd_SOURCE_DIR}/lib)
target_include_directories(zstd_seekable PRIVATE ${zstd_SOURCE_DIR}/lib/common)
target_link_libraries(zstd_seekable PUBLIC libzstd_static)

add_library(cgltf INTERFACE)
target_include_directories(cgltf SYSTEM INTERFACE ${cgltf_SOURCE_DIR})
add_library(stb INTERFACE)
target_include_directories(stb SYSTEM INTERFACE ${stb_SOURCE_DIR})
target_link_libraries(volk PUBLIC Vulkan::Headers)

# Dear ImGui and YAPF have no CMake of their own; build them here.
FetchContent_MakeAvailable(imgui yapf)
# One hook in ImGui: RenderFrame (the frame behind buttons, fields, checkboxes, combos) asks the
# engine's UI first, which draws Niveus's keys and wells instead (engine/ui/ui.cpp).
file(READ ${imgui_SOURCE_DIR}/imgui.cpp _imgui_cpp)
if(NOT _imgui_cpp MATCHES "GImRenderFrameHook")
  set(_from "void ImGui::RenderFrame(ImVec2 p_min, ImVec2 p_max, ImU32 fill_col, bool borders, float rounding)\n{\n    ImGuiContext& g = *GImGui;\n    ImGuiWindow* window = g.CurrentWindow;\n")
  string(FIND "${_imgui_cpp}" "${_from}" _at)
  if(_at EQUAL -1)
    message(FATAL_ERROR "ImGui's RenderFrame has changed: update the hook in cmake/Dependencies.cmake")
  endif()
  string(REPLACE "${_from}" "bool (*GImRenderFrameHook)(ImDrawList*, ImVec2, ImVec2, ImU32, bool, float) = nullptr;\n${_from}    if (GImRenderFrameHook && GImRenderFrameHook(window->DrawList, p_min, p_max, fill_col, borders, rounding))\n        return;\n" _imgui_cpp "${_imgui_cpp}")
  file(WRITE ${imgui_SOURCE_DIR}/imgui.cpp "${_imgui_cpp}")
endif()
add_library(imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/imgui_demo.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp) # (the crash screen)
target_include_directories(imgui SYSTEM PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_compile_definitions(imgui PUBLIC IMGUI_IMPL_VULKAN_USE_VOLK)
target_link_libraries(imgui PUBLIC volk SDL3::SDL3)
if(ANDROID) # its OpenGL backend calls GLES directly there
  target_link_libraries(imgui PUBLIC GLESv3 EGL)
endif()

add_library(yapf STATIC ${yapf_SOURCE_DIR}/yapf.c)
target_include_directories(yapf SYSTEM PUBLIC ${yapf_SOURCE_DIR})
find_package(Threads REQUIRED)
target_link_libraries(yapf PRIVATE Threads::Threads $<$<NOT:$<PLATFORM_ID:Windows>>:m>)
if(NOT MSVC)
  target_compile_options(yapf PRIVATE -O2) # its codecs and the BC7 encoder, even in debug builds
endif()

# Lua's core and the libraries scripts get (no io, os or loading modules: a script changes the
# scene, not the computer).
FetchContent_MakeAvailable(lua)
set(_lua_src lapi lcode lctype ldebug ldo ldump lfunc lgc llex lmem lobject lopcodes lparser lstate lstring ltable ltm lundump lvm lzio
             lauxlib lbaselib lcorolib ldblib lmathlib lstrlib ltablib lutf8lib)
list(TRANSFORM _lua_src PREPEND ${lua_SOURCE_DIR}/src/)
list(TRANSFORM _lua_src APPEND .c)
# As C++: its errors are then exceptions, which unwind through the bindings' C++ properly.
set_source_files_properties(${_lua_src} PROPERTIES LANGUAGE CXX)
add_library(lua STATIC ${_lua_src})
if(NOT MSVC)
  target_compile_options(lua PRIVATE -w)
endif()
target_include_directories(lua SYSTEM PUBLIC ${lua_SOURCE_DIR}/src)
if(NOT WIN32)
  target_link_libraries(lua PRIVATE m)
endif()

# Audio: miniaudio for output; Ogg Opus decoding (opusfile).
set(OPUS_BUILD_TESTING OFF)
set(OPUS_BUILD_PROGRAMS OFF)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF)
FetchContent_MakeAvailable(miniaudio ogg opus opusfile dejavu)
file(WRITE ${CMAKE_BINARY_DIR}/ogg-config/ogg/config_types.h
  "#pragma once\n#include <stdint.h>\ntypedef int16_t ogg_int16_t;\ntypedef uint16_t ogg_uint16_t;\ntypedef int32_t ogg_int32_t;\ntypedef uint32_t ogg_uint32_t;\ntypedef int64_t ogg_int64_t;\ntypedef uint64_t ogg_uint64_t;\n")
add_library(ogg STATIC ${ogg_SOURCE_DIR}/src/bitwise.c ${ogg_SOURCE_DIR}/src/framing.c)
target_include_directories(ogg SYSTEM PUBLIC ${ogg_SOURCE_DIR}/include ${CMAKE_BINARY_DIR}/ogg-config)
add_library(opusfile STATIC ${opusfile_SOURCE_DIR}/src/info.c ${opusfile_SOURCE_DIR}/src/internal.c
  ${opusfile_SOURCE_DIR}/src/opusfile.c ${opusfile_SOURCE_DIR}/src/stream.c)
target_include_directories(opusfile SYSTEM PUBLIC ${opusfile_SOURCE_DIR}/include)
target_compile_definitions(opusfile PRIVATE OP_DISABLE_HTTP OP_DISABLE_DOCS)
target_link_libraries(opusfile PUBLIC ogg Opus::opus)
add_library(miniaudio STATIC ${miniaudio_SOURCE_DIR}/miniaudio.c)
target_include_directories(miniaudio SYSTEM PUBLIC ${miniaudio_SOURCE_DIR})
target_link_libraries(miniaudio PUBLIC ${CMAKE_DL_LIBS} Threads::Threads)
if(UNIX AND NOT APPLE)
  target_link_libraries(miniaudio PUBLIC m)
endif()


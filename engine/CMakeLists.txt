# Fry3DX's engine, what a desktop tool needs: window, input, Vulkan/OpenGL renderer, ImGui, console and cvars, files,
# audio. No networking, physics, scripts or game framework.
add_library(keel STATIC
  core/log.cpp core/cvar.cpp core/console.cpp core/sysinfo.cpp
  fs/ymp.cpp fs/vfs.cpp
  audio/audio.cpp
  platform/window.cpp platform/input.cpp platform/touch.cpp
  rhi/vulkan/vk_device.cpp rhi/vulkan/vk_backend.cpp
  rhi/gl/gl_loader.cpp rhi/gl/gl_backend.cpp rhi/gl/gl_legacy.cpp
  render/renderer.cpp render/resources.cpp render/clusters.cpp render/screenshot.cpp
  ui/ui.cpp
  engine.cpp crash.cpp)
target_include_directories(keel PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
add_custom_target(keel_version COMMAND ${CMAKE_COMMAND} -DSRC=${PROJECT_SOURCE_DIR} -DOUT=${CMAKE_BINARY_DIR}/generated/keel_version.h
                  -P ${PROJECT_SOURCE_DIR}/cmake/Version.cmake BYPRODUCTS ${CMAKE_BINARY_DIR}/generated/keel_version.h VERBATIM)
add_dependencies(keel keel_version)
target_include_directories(keel PRIVATE ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(keel PRIVATE stb miniaudio opusfile)
if(ANDROID)
  target_link_libraries(keel PUBLIC log) # logcat
endif()
target_link_libraries(keel PUBLIC zstd_seekable glm::glm yapf SDL3::SDL3 volk vk-bootstrap::vk-bootstrap GPUOpen::VulkanMemoryAllocator imgui)
target_compile_definitions(keel PUBLIC GLM_FORCE_DEPTH_ZERO_TO_ONE GLM_ENABLE_EXPERIMENTAL VK_NO_PROTOTYPES)
keel_add_shaders(keel shaders/box.vert shaders/box.frag shaders/mesh.vert shaders/mesh.frag shaders/depth.frag shaders/cull.comp shaders/froxel.comp shaders/froxel_rt.comp shaders/gi.comp shaders/skinned.vert
                      shaders/fullscreen.vert shaders/tonemap.frag shaders/sky.vert shaders/sky.frag shaders/smoke_sky.frag shaders/hzb.frag
                      shaders/ssao.frag shaders/ssao_blur.frag shaders/bloom_down.frag shaders/bloom_up.frag shaders/taa.frag shaders/ssr.frag shaders/adapt.frag shaders/volumetric.frag shaders/sprite.vert shaders/sprite.frag)
if(MSVC)
  target_compile_options(keel PRIVATE /W4)
else()
  target_compile_options(keel PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
endif()
target_precompile_headers(keel PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/core/pch.h)

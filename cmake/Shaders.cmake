# keel_add_shaders(target file...) compiles GLSL to SPIR-V with glslc (KEEL_VULKAN defined) and
# embeds it: `static const uint32_t kSpv[] =\n#include "name.vert.spv.inc"\n;`
# It also embeds every shader's source text (and the shared headers) for the OpenGL backend,
# which compiles them at runtime with KEEL_GL defined: shaders/gl_sources.inc, a list of
# { "name", "source" } entries.
find_program(GLSLC glslc HINTS $ENV{VULKAN_SDK}/bin REQUIRED)

function(keel_add_shaders target)
  set(out_dir ${CMAKE_CURRENT_BINARY_DIR}/shaders)
  file(GLOB headers ${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.glsl)
  foreach(src ${ARGN})
    get_filename_component(name ${src} NAME)
    set(out ${out_dir}/${name}.spv.inc)
    add_custom_command(OUTPUT ${out}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${out_dir}
      COMMAND ${GLSLC} --target-env=vulkan1.3 -DKEEL_VULKAN -O -mfmt=c -I ${CMAKE_CURRENT_SOURCE_DIR}/shaders -o ${out} ${CMAKE_CURRENT_SOURCE_DIR}/${src}
      DEPENDS ${src} ${headers} VERBATIM)
    list(APPEND outs ${out})
    list(APPEND sources ${CMAKE_CURRENT_SOURCE_DIR}/${src})
  endforeach()
  set(gl_out ${out_dir}/gl_sources.inc)
  string(REPLACE ";" "|" embed_list "${sources};${headers}") # a list can't pass through COMMAND as one argument
  add_custom_command(OUTPUT ${gl_out}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${out_dir}
    COMMAND ${CMAKE_COMMAND} -DOUT=${gl_out} -DFILES=${embed_list} -P ${PROJECT_SOURCE_DIR}/cmake/EmbedText.cmake
    DEPENDS ${sources} ${headers} ${PROJECT_SOURCE_DIR}/cmake/EmbedText.cmake VERBATIM)
  target_sources(${target} PRIVATE ${outs} ${gl_out})
  target_include_directories(${target} PRIVATE ${out_dir})
endfunction()

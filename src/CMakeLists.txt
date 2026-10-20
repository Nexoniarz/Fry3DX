# Fry3DX: the app (app.cpp) on the engine, and its modelling core (mesh, scene) as a library the
# tests use too.
add_library(fry_core STATIC mesh.cpp tools.cpp cut.cpp uv.cpp image.cpp sticker.cpp shapes.cpp scene.cpp package.cpp export.cpp print.cpp boolean.cpp modify.cpp)
target_include_directories(fry_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fry_core PUBLIC glm::glm libzstd_static)
target_compile_definitions(fry_core PUBLIC GLM_ENABLE_EXPERIMENTAL)
# Scripts (Lua) that build and change the scene: also without a window, for the tests.
add_library(fry_script STATIC script.cpp)
target_link_libraries(fry_script PUBLIC fry_core PRIVATE lua)

if(ANDROID) # SDL's Java side loads the app as libmain.so and calls its (SDL_)main
  add_library(fry3dx SHARED app.cpp icons.cpp)
  set_target_properties(fry3dx PROPERTIES OUTPUT_NAME main LIBRARY_OUTPUT_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
else()
  add_executable(fry3dx app.cpp icons.cpp)
endif()
if(WIN32) # a window program (no console window), with its icon
  target_sources(fry3dx PRIVATE fry3dx.rc)
  set_target_properties(fry3dx PROPERTIES WIN32_EXECUTABLE ON)
  target_link_options(fry3dx PRIVATE /ENTRY:mainCRTStartup)
endif()
target_link_libraries(fry3dx PRIVATE fry_core fry_script keel stb)
add_dependencies(fry3dx content)
foreach(t fry_core fry_script fry3dx)
  if(MSVC)
    target_compile_options(${t} PRIVATE /W3)
  else()
    target_compile_options(${t} PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
  endif()
endforeach()

# Fry3DX: the app (app.cpp) on the engine, and its modelling core (mesh, scene) as a library the
# tests use too.
add_library(fry_core STATIC mesh.cpp tools.cpp cut.cpp uv.cpp image.cpp sticker.cpp shapes.cpp scene.cpp package.cpp export.cpp print.cpp boolean.cpp modify.cpp)
target_include_directories(fry_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fry_core PUBLIC glm::glm libzstd_static)
target_compile_definitions(fry_core PUBLIC GLM_ENABLE_EXPERIMENTAL)

add_executable(fry3dx app.cpp icons.cpp)
target_link_libraries(fry3dx PRIVATE fry_core keel stb)
add_dependencies(fry3dx content)
foreach(t fry_core fry3dx)
  target_compile_options(${t} PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
endforeach()

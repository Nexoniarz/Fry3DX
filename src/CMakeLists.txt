# Fry3DX: the app (app.cpp) on the engine, and its modelling core (mesh, scene) as a library the
# tests use too.
# Manifold (github.com/elalish/manifold): exact, robust combining of closed shapes. Just the
# library: no threads (TBB), no 2D, no bindings.
include(FetchContent)
set(MANIFOLD_PAR OFF CACHE BOOL "" FORCE)
set(MANIFOLD_CROSS_SECTION OFF CACHE BOOL "" FORCE)
set(MANIFOLD_TEST OFF CACHE BOOL "" FORCE)
set(MANIFOLD_PYBIND OFF CACHE BOOL "" FORCE)
set(MANIFOLD_CBIND OFF CACHE BOOL "" FORCE)
set(MANIFOLD_JSBIND OFF CACHE BOOL "" FORCE)
set(MANIFOLD_EXPORT OFF CACHE BOOL "" FORCE)
FetchContent_Declare(manifold GIT_REPOSITORY https://github.com/elalish/manifold.git
                     GIT_TAG v3.0.1 GIT_SHALLOW ON SYSTEM)
FetchContent_MakeAvailable(manifold)

add_library(fry_core STATIC mesh.cpp bvh.cpp text.cpp import.cpp tools.cpp cut.cpp uv.cpp image.cpp sticker.cpp shapes.cpp scene.cpp package.cpp export.cpp print.cpp boolean.cpp modify.cpp sculpt.cpp)
target_include_directories(fry_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fry_core PUBLIC glm::glm libzstd_static PRIVATE stb cgltf manifold)
target_compile_definitions(fry_core PUBLIC GLM_ENABLE_EXPERIMENTAL)
# Scripts (Lua) that build and change the scene: also without a window, for the tests.
add_library(fry_script STATIC script.cpp)
target_link_libraries(fry_script PUBLIC fry_core PRIVATE lua)

# The app: its class declared in app.h, what it does in app*.cpp by part.
set(FRY_APP_SOURCES app.cpp app_model.cpp app_paint.cpp app_panels.cpp app_print.cpp app_scripts.cpp app_view.cpp commands.cpp icons.cpp)
if(ANDROID) # SDL's Java side loads the app as libmain.so and calls its (SDL_)main
  add_library(fry3dx SHARED ${FRY_APP_SOURCES})
  set_target_properties(fry3dx PROPERTIES OUTPUT_NAME main LIBRARY_OUTPUT_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
elseif(APPLE) # a Mac app: Fry3DX.app, content in Contents/Resources, MoltenVK in Contents/Frameworks
  set(icns ${PROJECT_SOURCE_DIR}/branding/fry3dx.icns)
  add_executable(fry3dx MACOSX_BUNDLE ${FRY_APP_SOURCES} ${icns})
  set_source_files_properties(${icns} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
  set_target_properties(fry3dx PROPERTIES OUTPUT_NAME Fry3DX MACOSX_BUNDLE_INFO_PLIST ${PROJECT_SOURCE_DIR}/cmake/Info.plist.in)
  # Vulkan on a Mac is MoltenVK (Vulkan on Metal): FRY_MOLTENVK=/path/to/libMoltenVK.dylib, or
  # found (Homebrew's molten-vk, a Vulkan SDK). Without it, the app falls back to OpenGL 4.1.
  find_library(FRY_MOLTENVK NAMES libMoltenVK.dylib MoltenVK HINTS $ENV{VULKAN_SDK}/lib /opt/homebrew/lib /usr/local/lib NO_CMAKE_FIND_ROOT_PATH)
  if(FRY_MOLTENVK)
    add_custom_command(TARGET fry3dx POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_BUNDLE_CONTENT_DIR:fry3dx>/Frameworks
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FRY_MOLTENVK} $<TARGET_BUNDLE_CONTENT_DIR:fry3dx>/Frameworks/libMoltenVK.dylib
      VERBATIM)
  else()
    message(WARNING "No MoltenVK found (set FRY_MOLTENVK): Fry3DX.app will only have OpenGL 4.1")
  endif()
else()
  add_executable(fry3dx ${FRY_APP_SOURCES})
endif()
if(WIN32) # a window program (no console window), with its icon
  target_sources(fry3dx PRIVATE ${CMAKE_BINARY_DIR}/generated/fry3dx.rc)
  set_target_properties(fry3dx PROPERTIES WIN32_EXECUTABLE ON)
  target_link_options(fry3dx PRIVATE /ENTRY:mainCRTStartup)
endif()
target_link_libraries(fry3dx PRIVATE fry_core fry_script keel stb)
fry_project_header(${CMAKE_BINARY_DIR}/generated/fry_project.h ${CMAKE_BINARY_DIR}/generated/fry3dx.rc) # (project.json)
target_include_directories(fry3dx PRIVATE ${CMAKE_BINARY_DIR}/generated)
add_dependencies(fry3dx content)
foreach(t fry_core fry_script fry3dx)
  if(MSVC)
    target_compile_options(${t} PRIVATE /W3)
  else()
    target_compile_options(${t} PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
  endif()
endforeach()

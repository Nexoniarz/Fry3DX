# Fry3DX: the app (app.cpp) on the engine, and its modelling core (mesh, scene) as a library the
# tests use too.
add_library(fry_core STATIC mesh.cpp bvh.cpp tools.cpp cut.cpp uv.cpp image.cpp sticker.cpp shapes.cpp scene.cpp package.cpp export.cpp print.cpp boolean.cpp modify.cpp sculpt.cpp)
target_include_directories(fry_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fry_core PUBLIC glm::glm libzstd_static)
target_compile_definitions(fry_core PUBLIC GLM_ENABLE_EXPERIMENTAL)
# Scripts (Lua) that build and change the scene: also without a window, for the tests.
add_library(fry_script STATIC script.cpp)
target_link_libraries(fry_script PUBLIC fry_core PRIVATE lua)

if(ANDROID) # SDL's Java side loads the app as libmain.so and calls its (SDL_)main
  add_library(fry3dx SHARED app.cpp commands.cpp icons.cpp)
  set_target_properties(fry3dx PROPERTIES OUTPUT_NAME main LIBRARY_OUTPUT_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
elseif(APPLE) # a Mac app: Fry3DX.app, content in Contents/Resources, MoltenVK in Contents/Frameworks
  set(icns ${PROJECT_SOURCE_DIR}/branding/fry3dx.icns)
  add_executable(fry3dx MACOSX_BUNDLE app.cpp commands.cpp icons.cpp ${icns})
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
  add_executable(fry3dx app.cpp commands.cpp icons.cpp)
endif()
if(WIN32) # a window program (no console window), with its icon
  target_sources(fry3dx PRIVATE fry3dx.rc)
  set_target_properties(fry3dx PROPERTIES WIN32_EXECUTABLE ON)
  target_link_options(fry3dx PRIVATE /ENTRY:mainCRTStartup)
endif()
target_link_libraries(fry3dx PRIVATE fry_core fry_script keel stb)
target_compile_definitions(fry3dx PRIVATE FRY_VERSION="${PROJECT_VERSION}")
add_dependencies(fry3dx content)
foreach(t fry_core fry_script fry3dx)
  if(MSVC)
    target_compile_options(${t} PRIVATE /W3)
  else()
    target_compile_options(${t} PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
  endif()
endforeach()

add_executable(mesh_test mesh_test.cpp)
target_link_libraries(mesh_test PRIVATE fry_core)
add_test(NAME mesh_test COMMAND mesh_test WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

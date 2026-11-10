# Headless tests: no window, no GPU.
add_executable(zl_tests test_main.cpp)
target_link_libraries(zl_tests PRIVATE zl_actor zl_art zl_options)
# The art tests load the painted assets from the source tree.
target_compile_definitions(zl_tests PRIVATE ZL_ASSET_DIR="${PROJECT_SOURCE_DIR}/assets")
add_test(NAME zl_tests COMMAND zl_tests)

add_executable(zl_collision_tests test_collision.cpp)
target_link_libraries(zl_collision_tests PRIVATE zl_actor zl_options)
add_test(NAME zl_collision_tests COMMAND zl_collision_tests)

if(ZL_BUILD_APP)
  # Explicit opt-in: run under Xvfb (or a real display). Ordinary CTest stays
  # headless and does not require a display server.
  add_executable(zl_render_tests render_test.cpp)
  target_link_libraries(zl_render_tests PRIVATE zl_render zl_options)
  target_compile_definitions(zl_render_tests PRIVATE ZL_ASSET_DIR="${PROJECT_SOURCE_DIR}/assets")
  add_executable(zl_figure_render_tests test_figure_render.cpp)
  target_link_libraries(zl_figure_render_tests PRIVATE zl_render zl_options)
endif()

# Headless tests: no window, no GPU.
add_executable(zl_tests test_main.cpp)
target_link_libraries(zl_tests PRIVATE zl_actor zl_art zl_options)
# The art tests load the painted assets from the source tree.
target_compile_definitions(zl_tests PRIVATE ZL_ASSET_DIR="${PROJECT_SOURCE_DIR}/assets")
add_test(NAME zl_tests COMMAND zl_tests)

if(ZL_BUILD_APP)
  # GPU checks need a display (or xvfb-run); keep ctest itself headless.
  add_executable(zl_render_tests test_figure_render.cpp ${PROJECT_SOURCE_DIR}/game/render/figure_renderer.cpp)
  target_link_libraries(zl_render_tests PRIVATE zl_actor zl_gfx zl_options)
endif()

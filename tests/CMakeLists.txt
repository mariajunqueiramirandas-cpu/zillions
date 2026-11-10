# Headless tests: no window, no GPU.
add_executable(zl_tests test_main.cpp)
target_link_libraries(zl_tests PRIVATE zl_art zl_options)
# The art tests load the painted assets from the source tree.
target_compile_definitions(zl_tests PRIVATE ZL_ASSET_DIR="${PROJECT_SOURCE_DIR}/assets")
add_test(NAME zl_tests COMMAND zl_tests)

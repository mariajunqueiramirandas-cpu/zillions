# Headless tests: no window, no GPU.
add_executable(zl_tests test_main.cpp)
target_link_libraries(zl_tests PRIVATE zl_world zl_options)
add_test(NAME zl_tests COMMAND zl_tests)

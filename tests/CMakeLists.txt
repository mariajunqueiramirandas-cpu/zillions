# Headless tests: no window, no GPU.
add_executable(zl_biome_tests test_biomes.cpp)
target_link_libraries(zl_biome_tests PRIVATE zl_actor zl_options)
add_test(NAME zl_biome_tests COMMAND zl_biome_tests)

add_executable(zl_tests test_main.cpp)
target_link_libraries(zl_tests PRIVATE zl_actor zl_art zl_options)
# The art tests load the painted assets from the source tree.
target_compile_definitions(zl_tests PRIVATE ZL_ASSET_DIR="${PROJECT_SOURCE_DIR}/assets")
add_test(NAME zl_tests COMMAND zl_tests)

add_executable(zl_collision_tests test_collision.cpp)
target_link_libraries(zl_collision_tests PRIVATE zl_actor zl_options)
add_test(NAME zl_collision_tests COMMAND zl_collision_tests)

add_executable(zl_survival_tests test_survival.cpp)
target_link_libraries(zl_survival_tests PRIVATE zl_actor zl_options)
add_test(NAME zl_survival_tests COMMAND zl_survival_tests)

# glTF loading and the plague doctor's animation state machine.
add_executable(zl_model_tests test_gltf.cpp)
target_link_libraries(zl_model_tests PRIVATE zl_actor zl_options)
target_compile_definitions(zl_model_tests PRIVATE ZL_ASSET_DIR="${PROJECT_SOURCE_DIR}/assets")
add_test(NAME zl_model_tests COMMAND zl_model_tests)

add_executable(zl_plague_tests test_plague.cpp)
target_link_libraries(zl_plague_tests PRIVATE zl_actor zl_options)
target_compile_definitions(zl_plague_tests PRIVATE ZL_ASSET_DIR="${PROJECT_SOURCE_DIR}/assets")
add_test(NAME zl_plague_tests COMMAND zl_plague_tests)

add_executable(zl_scenery_tests test_scenery_models.cpp)
target_link_libraries(zl_scenery_tests PRIVATE zl_actor zl_options)
target_compile_definitions(zl_scenery_tests PRIVATE ZL_ASSET_DIR="${PROJECT_SOURCE_DIR}/assets")
add_test(NAME zl_scenery_tests COMMAND zl_scenery_tests)

add_executable(zl_openrouter_tests test_openrouter.cpp)
target_link_libraries(zl_openrouter_tests PRIVATE zl_ai zl_net nlohmann_json::nlohmann_json zl_options)
if(WIN32)
  target_link_libraries(zl_openrouter_tests PRIVATE ws2_32)
endif()
add_test(NAME zl_openrouter_tests COMMAND zl_openrouter_tests)

if(ZL_BUILD_APP)
  add_executable(zl_terrain_material_tests test_terrain_material.cpp)
  target_link_libraries(zl_terrain_material_tests PRIVATE zl_render zl_options)
  add_executable(zl_foliage_render_tests test_foliage_render.cpp)
  target_link_libraries(zl_foliage_render_tests PRIVATE zl_render zl_options)
  target_compile_definitions(zl_foliage_render_tests PRIVATE ZL_ASSET_DIR="${PROJECT_SOURCE_DIR}/assets")
  add_executable(zl_lighting_tests test_lighting.cpp)
  target_link_libraries(zl_lighting_tests PRIVATE zl_render zl_options)
  add_executable(zl_scenery_render_tests test_scenery_render.cpp)
  target_link_libraries(zl_scenery_render_tests PRIVATE zl_render zl_options)
  target_compile_definitions(zl_scenery_render_tests PRIVATE ZL_ASSET_DIR="${PROJECT_SOURCE_DIR}/assets")
  add_executable(zl_survival_render_tests survival_render_test.cpp)
  target_link_libraries(zl_survival_render_tests PRIVATE zl_render zl_options)
  target_compile_definitions(zl_survival_render_tests PRIVATE ZL_ASSET_DIR="${PROJECT_SOURCE_DIR}/assets")
  # Explicit opt-in: run under Xvfb (or a real display). Ordinary CTest stays
  # headless and does not require a display server.
  add_executable(zl_render_tests render_test.cpp)
  target_link_libraries(zl_render_tests PRIVATE zl_render zl_options)
  target_compile_definitions(zl_render_tests PRIVATE ZL_ASSET_DIR="${PROJECT_SOURCE_DIR}/assets")
  add_executable(zl_figure_render_tests test_figure_render.cpp)
  target_link_libraries(zl_figure_render_tests PRIVATE zl_render zl_options)
  add_executable(zl_model_render_tests test_model_render.cpp)
  target_link_libraries(zl_model_render_tests PRIVATE zl_render zl_options)
  target_compile_definitions(zl_model_render_tests PRIVATE ZL_ASSET_DIR="${PROJECT_SOURCE_DIR}/assets")
endif()

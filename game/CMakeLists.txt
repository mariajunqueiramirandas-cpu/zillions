# World model and generation: headless, depends only on the engine core.
add_library(zl_world STATIC
  world/biome.cpp
  world/map_preview.cpp
  world/mapgen.cpp
  world/scenery.cpp
  world/tilemap.cpp)
target_include_directories(zl_world PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(zl_world PUBLIC zl_core PRIVATE zl_options)

# Map art: reads the painted textures and sprites, or draws placeholders in
# code. CPU only, so the tests can check it without a GPU.
add_library(zl_art STATIC render/terrain_art.cpp)
target_link_libraries(zl_art PUBLIC zl_world PRIVATE zl_options)

# glTF/GLB models: a headless loader with embedded image decoding. Used by
# the plague doctor's animation state machine and the GPU model renderer.
add_library(zl_assets STATIC asset/gltf.cpp)
target_include_directories(zl_assets PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(zl_assets PUBLIC zl_core PRIVATE nlohmann_json::nlohmann_json stb zl_options)

# Characters: the figure description, the human rig that builds one in code,
# the plague doctor's animation state machine and the player's controller. CPU
# only and deterministic, like the world.
add_library(zl_actor STATIC
  actor/figure.cpp
  actor/human.cpp
  actor/player.cpp
  actor/plague.cpp
  survival/survival.cpp)
target_link_libraries(zl_actor PUBLIC zl_world zl_assets PRIVATE zl_options)

# OpenRouter chat client: HTTP and JSON only, with no gameplay integration yet.
add_library(zl_ai STATIC ai/openrouter.cpp)
target_include_directories(zl_ai PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(zl_ai PUBLIC zl_core zl_net PRIVATE nlohmann_json::nlohmann_json zl_options)

# Headless map generator: previews, statistics and benchmarks without a GPU.
add_executable(zl_mapgen ${PROJECT_SOURCE_DIR}/tools/mapgen_cli.cpp)
target_link_libraries(zl_mapgen PRIVATE zl_world zl_options)

if(ZL_BUILD_APP)
  add_library(zl_render STATIC
    render/figure_renderer.cpp
    render/model_renderer.cpp
    render/scenery_renderer.cpp
    render/survival_ui.cpp
    render/map_renderer.cpp
    render/relief.cpp)
  target_link_libraries(zl_render PUBLIC zl_actor zl_art zl_gfx PRIVATE zl_options)
  target_compile_definitions(zl_render PRIVATE ZL_ASSET_DIR="${PROJECT_SOURCE_DIR}/assets")
  add_executable(zillions app/main.cpp)
  target_link_libraries(zillions PRIVATE zl_render zl_options)
  # Lets the viewer find the painted art when it is not run from the repository root.
  target_compile_definitions(zillions PRIVATE ZL_ASSET_DIR="${PROJECT_SOURCE_DIR}/assets")
endif()

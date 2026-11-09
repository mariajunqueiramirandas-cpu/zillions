# World model and generation: headless, depends only on the engine core.
add_library(zl_world STATIC
  world/map_preview.cpp
  world/mapgen.cpp
  world/tilemap.cpp)
target_include_directories(zl_world PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(zl_world PUBLIC zl_core PRIVATE zl_options)

# Headless map generator: previews, statistics and benchmarks without a GPU.
add_executable(zl_mapgen ${PROJECT_SOURCE_DIR}/tools/mapgen_cli.cpp)
target_link_libraries(zl_mapgen PRIVATE zl_world zl_options)

if(ZL_BUILD_APP)
  add_executable(zillions
    app/main.cpp
    render/map_renderer.cpp
    render/relief.cpp
    render/terrain_art.cpp)
  target_link_libraries(zillions PRIVATE zl_world zl_gfx zl_options)
endif()

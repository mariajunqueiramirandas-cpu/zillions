find_package(Threads REQUIRED)

# Platform-independent core: no windowing, no GPU. Everything the simulation
# needs lives here so it can run headless (tests, tools, dedicated servers).
add_library(zl_core STATIC
  core/image.cpp
  core/jobs.cpp)
target_include_directories(zl_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(zl_core PUBLIC Threads::Threads PRIVATE stb zl_options)

if(ZL_BUILD_APP)
  set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.4
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(glfw)

  # OpenGL 3.3 core loader, generated with glad 2 and checked in.
  add_library(glad STATIC ${PROJECT_SOURCE_DIR}/third_party/glad/src/gl.c)
  target_include_directories(glad SYSTEM PUBLIC ${PROJECT_SOURCE_DIR}/third_party/glad/include)

  add_library(zl_gfx STATIC
    gfx/render_target.cpp
    gfx/shader.cpp
    gfx/sprite_batch.cpp
    gfx/texture.cpp
    platform/window.cpp)
  target_link_libraries(zl_gfx PUBLIC zl_core glad PRIVATE glfw zl_options)
endif()

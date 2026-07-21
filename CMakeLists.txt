cmake_minimum_required(VERSION 3.26)
project(mediator CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

if(MSVC)
  add_compile_options(/W4 /WX- /permissive-)
else()
  add_compile_options(-Wall -Wextra -g)
endif()

include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.15.2
  GIT_SHALLOW TRUE
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_library(mediator_core STATIC
  src/core/clock.cpp
  src/core/priority_timer.cpp
  src/core/reactor.cpp
  src/engine/message.cpp
  src/engine/blackboard.cpp
  src/engine/heartbeat_engine.cpp
  src/audio/g711.cpp
  src/audio/watermark.cpp
  src/audio/align.cpp
  src/session/context_evict.cpp
  src/ext/auth_provider.cpp
)
target_include_directories(mediator_core PUBLIC src)

add_executable(mediator src/main.cpp)
target_link_libraries(mediator PRIVATE mediator_core)

enable_testing()
add_subdirectory(tests)

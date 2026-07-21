cmake_minimum_required(VERSION 3.22)
project(mediator CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

if(MSVC)
  add_compile_options(/W4 /permissive-)
else()
  add_compile_options(-Wall -Wextra -g)
endif()

# ---- spdlog：优先系统包，否则 FetchContent ----
find_package(spdlog QUIET)
if(NOT spdlog_FOUND)
  include(FetchContent)
  FetchContent_Declare(spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.14.1 GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(spdlog)
endif()

# ---- googletest ----
include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.15.2
  GIT_SHALLOW TRUE
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# ---- wasm3（third_party 内置源码）----
file(GLOB WASM3_SOURCES third_party/wasm3/source/*.c)
add_library(wasm3 STATIC ${WASM3_SOURCES})
target_include_directories(wasm3 PUBLIC third_party/wasm3/source)
if(UNIX)
  target_link_libraries(wasm3 PUBLIC m)
endif()

# ---- 核心库（无外部重依赖，Windows/Linux 均可构建）----
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
  src/ext/hs256.cpp
  src/ext/wasm3_host.cpp
  src/ext/wasm_bus.cpp
)
target_include_directories(mediator_core PUBLIC src)
target_link_libraries(mediator_core PUBLIC spdlog::spdlog wasm3)

# ---- gRPC/Protobuf（Linux/WSL；Windows 跳过网络层与工具）----
if(UNIX)
  find_package(Protobuf REQUIRED)
  find_program(GRPC_CPP_PLUGIN grpc_cpp_plugin REQUIRED)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(GRPCPP REQUIRED grpc++)
  pkg_check_modules(WEBRTC_APM REQUIRED webrtc-audio-processing)
  pkg_check_modules(HIREDIS REQUIRED hiredis)

  set(PROTO_GEN ${CMAKE_BINARY_DIR}/proto_gen)
  file(MAKE_DIRECTORY ${PROTO_GEN})
  add_custom_command(
    OUTPUT ${PROTO_GEN}/mediator.pb.cc ${PROTO_GEN}/mediator.pb.h
           ${PROTO_GEN}/mediator.grpc.pb.cc ${PROTO_GEN}/mediator.grpc.pb.h
    COMMAND ${Protobuf_PROTOC_EXECUTABLE}
      --proto_path=${CMAKE_SOURCE_DIR}/proto
      --cpp_out=${PROTO_GEN}
      --grpc_out=${PROTO_GEN}
      --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}
      ${CMAKE_SOURCE_DIR}/proto/mediator.proto
    DEPENDS ${CMAKE_SOURCE_DIR}/proto/mediator.proto)

  add_library(mediator_net STATIC
    ${PROTO_GEN}/mediator.pb.cc
    ${PROTO_GEN}/mediator.grpc.pb.cc
    src/net/grpc_clients.cpp
    src/net/ws_server.cpp
    src/gateway.cpp
    src/audio/apm_wrapper.cpp
    src/session/redis_store.cpp
    src/telemetry/telemetry.cpp
    src/crash/crash_handler.cpp
  )
  target_include_directories(mediator_net PUBLIC ${PROTO_GEN}
    ${GRPCPP_INCLUDE_DIRS} ${WEBRTC_APM_INCLUDE_DIRS} ${HIREDIS_INCLUDE_DIRS})
  target_compile_options(mediator_net PUBLIC ${WEBRTC_APM_CFLAGS_OTHER})
  target_link_directories(mediator_net PUBLIC ${GRPCPP_LIBRARY_DIRS}
    ${WEBRTC_APM_LIBRARY_DIRS} ${HIREDIS_LIBRARY_DIRS})
  target_link_libraries(mediator_net PUBLIC mediator_core
    ${GRPCPP_LIBRARIES} ${WEBRTC_APM_LIBRARIES} ${HIREDIS_LIBRARIES}
    protobuf ssl crypto pthread)

  add_executable(mediator src/main.cpp)
  target_link_libraries(mediator PRIVATE mediator_net)

  # 符号流水线（§7B.2）：抽调试符号 + strip + debuglink 关联
  # （部分 binutils 的 strip 不支持 --add-gnu-debuglink，统一用 objcopy）
  add_custom_target(symbols
    COMMAND objcopy --only-keep-debug mediator mediator.debug
    COMMAND objcopy --strip-debug --add-gnu-debuglink=mediator.debug mediator
    DEPENDS mediator
    COMMENT "extract debug symbols -> mediator.debug (addr2line/minidump_stackwalk)")

  add_subdirectory(tools)
else()
  add_executable(mediator src/main.cpp)
  target_link_libraries(mediator PRIVATE mediator_core)
endif()

enable_testing()
add_subdirectory(tests)

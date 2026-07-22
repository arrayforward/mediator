# ---- wat → wasm（有 wat2wasm 时构建期生成；否则相关用例 GTEST_SKIP）----
find_program(WAT2WASM wat2wasm)
set(WASM_OUT_DIR ${CMAKE_BINARY_DIR}/tests/wasm)
file(MAKE_DIRECTORY ${WASM_OUT_DIR})
if(WAT2WASM)
  foreach(m auth_allow auth_trap msg_counter msg_trap convai_proto)
    add_custom_command(
      OUTPUT ${WASM_OUT_DIR}/${m}.wasm
      COMMAND ${WAT2WASM} ${CMAKE_CURRENT_SOURCE_DIR}/wasm/${m}.wat -o ${WASM_OUT_DIR}/${m}.wasm
      DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/wasm/${m}.wat)
    list(APPEND WASM_BINS ${WASM_OUT_DIR}/${m}.wasm)
  endforeach()
  add_custom_target(wasm_test_modules ALL DEPENDS ${WASM_BINS})
endif()

add_executable(mediator_tests
  test_channel.cpp
  test_timer.cpp
  test_reactor.cpp
  test_g711.cpp
  test_watermark.cpp
  test_align.cpp
  test_context_evict.cpp
  test_auth.cpp
  test_heartbeat.cpp
  test_wasm3.cpp
  test_wasm_bus.cpp
)
if(UNIX)
  target_sources(mediator_tests PRIVATE test_telemetry.cpp test_protocol_plugin.cpp)
  target_link_libraries(mediator_tests PRIVATE mediator_net nlohmann_json::nlohmann_json)
endif()
target_compile_definitions(mediator_tests PRIVATE MEDIATOR_WASM_DIR="${WASM_OUT_DIR}")
target_link_libraries(mediator_tests PRIVATE mediator_core GTest::gtest_main)
if(TARGET wasm_test_modules)
  add_dependencies(mediator_tests wasm_test_modules)
endif()
include(GoogleTest)
gtest_discover_tests(mediator_tests)

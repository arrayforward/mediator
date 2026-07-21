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
)
target_link_libraries(mediator_tests PRIVATE mediator_core GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(mediator_tests)

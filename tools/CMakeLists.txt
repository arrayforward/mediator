add_executable(mock_services mock_services/main.cpp)
target_link_libraries(mock_services PRIVATE mediator_net)

add_executable(e2e_client e2e_client/main.cpp)
target_link_libraries(e2e_client PRIVATE mediator_net ssl crypto)

add_executable(convai_sim convai_sim/main.cpp)
target_link_libraries(convai_sim PRIVATE mediator_net ssl crypto)

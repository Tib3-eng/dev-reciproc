cmake_minimum_required(VERSION 3.20)
project(EncoderCore LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_library(encoder_state STATIC src/encoder_state.c)
target_include_directories(encoder_state PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(encoder_control_protocol STATIC src/encoder_control_protocol.c)
target_include_directories(encoder_control_protocol PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(encoder_control_protocol PUBLIC encoder_state)

add_library(recip_encoder_controller STATIC src/recip_encoder_controller.c)
target_include_directories(recip_encoder_controller PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(recip_encoder_controller PUBLIC encoder_control_protocol)

add_executable(encoder_state_tests tests/encoder_state_tests.c)
target_link_libraries(encoder_state_tests PRIVATE encoder_state)

add_executable(encoder_state_probe tools/encoder_state_probe.c)
target_link_libraries(encoder_state_probe PRIVATE encoder_state)

add_executable(encoder_control_protocol_tests tests/encoder_control_protocol_tests.c)
target_link_libraries(encoder_control_protocol_tests PRIVATE encoder_control_protocol)

add_executable(recip_encoder_controller_tests tests/recip_encoder_controller_tests.c)
target_link_libraries(recip_encoder_controller_tests PRIVATE recip_encoder_controller)

add_executable(recip_encoder_replay tools/recip_encoder_replay.c)
target_link_libraries(recip_encoder_replay PRIVATE recip_encoder_controller)

if(MSVC)
  target_compile_options(encoder_state PRIVATE /W4)
  target_compile_options(encoder_state_tests PRIVATE /W4)
  target_compile_options(encoder_state_probe PRIVATE /W4)
  target_compile_options(encoder_control_protocol PRIVATE /W4)
  target_compile_options(encoder_control_protocol_tests PRIVATE /W4)
  target_compile_options(recip_encoder_controller PRIVATE /W4)
  target_compile_options(recip_encoder_controller_tests PRIVATE /W4)
  target_compile_options(recip_encoder_replay PRIVATE /W4)
else()
  target_compile_options(encoder_state PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(encoder_state_tests PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(encoder_state_probe PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(encoder_control_protocol PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(encoder_control_protocol_tests PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(recip_encoder_controller PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(recip_encoder_controller_tests PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(recip_encoder_replay PRIVATE -Wall -Wextra -Wpedantic)
  target_link_libraries(encoder_state PUBLIC m)
endif()

enable_testing()
add_test(NAME encoder_state_unit COMMAND encoder_state_tests)
add_test(NAME encoder_control_protocol_unit COMMAND encoder_control_protocol_tests)
add_test(NAME recip_encoder_controller_unit COMMAND recip_encoder_controller_tests)

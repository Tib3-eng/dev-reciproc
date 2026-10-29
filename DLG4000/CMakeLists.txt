cmake_minimum_required(VERSION 3.20)
project(DLG4000 LANGUAGES C)

# MSVC: runtime estatico (/MT) para exe portavel
if (MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_executable(DLGlogger src/DLGlogger.c)
target_compile_definitions(DLGlogger PRIVATE WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
target_link_libraries(DLGlogger PRIVATE ws2_32)

add_executable(dlg_logger_ipc
  src/dlg_logger_ipc.c
  src/dlg_sample_clock.c
  src/dlg_encoder_stream.c
  ../EncoderCore/src/encoder_state.c
  ../EncoderCore/src/encoder_control_protocol.c
)
target_include_directories(dlg_logger_ipc PRIVATE ../EncoderCore/include)
target_compile_definitions(dlg_logger_ipc PRIVATE WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
target_link_libraries(dlg_logger_ipc PRIVATE ws2_32 winmm)
if (MSVC)
  target_compile_options(dlg_logger_ipc PRIVATE /W4)
endif()

add_executable(dlg_sample_clock_tests tests/dlg_sample_clock_tests.c src/dlg_sample_clock.c
  ../EncoderCore/src/encoder_state.c
  ../EncoderCore/src/encoder_control_protocol.c
  ../EncoderCore/src/recip_encoder_controller.c)
target_include_directories(dlg_sample_clock_tests PRIVATE src ../EncoderCore/include)
if(MSVC)
  target_compile_options(dlg_sample_clock_tests PRIVATE /W4)
endif()
enable_testing()
add_test(NAME dlg_sample_clock_unit COMMAND dlg_sample_clock_tests)

add_executable(dlg_encoder_stream_tests tests/dlg_encoder_stream_tests.c
  src/dlg_encoder_stream.c src/dlg_sample_clock.c
  ../EncoderCore/src/encoder_state.c ../EncoderCore/src/encoder_control_protocol.c)
target_include_directories(dlg_encoder_stream_tests PRIVATE src ../EncoderCore/include)
target_compile_definitions(dlg_encoder_stream_tests PRIVATE
  DLG_STREAM_TESTING STREAM_QUEUE_CAPACITY=64 _CRT_SECURE_NO_WARNINGS)
target_link_libraries(dlg_encoder_stream_tests PRIVATE ws2_32)
if(MSVC)
  target_compile_options(dlg_encoder_stream_tests PRIVATE /W4)
endif()
file(MAKE_DIRECTORY "${CMAKE_SOURCE_DIR}/../out/fifo_fix_20261027/stream_tests")
add_test(NAME dlg_encoder_stream_unit COMMAND dlg_encoder_stream_tests
  "${CMAKE_SOURCE_DIR}/../out/fifo_fix_20261027/stream_tests")

add_executable(dlg_encoder_test src/dlg_encoder_test.c)
target_compile_definitions(dlg_encoder_test PRIVATE WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
target_link_libraries(dlg_encoder_test PRIVATE ws2_32)
if (MSVC)
  target_compile_options(dlg_encoder_test PRIVATE /W4)
endif()

# Saida sempre em bin/
set_target_properties(DLGlogger PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
)
set_target_properties(dlg_logger_ipc PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
)
set_target_properties(dlg_encoder_test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
)

# Instala em bin/
install(TARGETS DLGlogger RUNTIME DESTINATION bin)
install(TARGETS dlg_logger_ipc RUNTIME DESTINATION bin)
install(TARGETS dlg_encoder_test RUNTIME DESTINATION bin)

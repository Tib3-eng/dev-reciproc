cmake_minimum_required(VERSION 3.20)
project(DLG4000 LANGUAGES C)

# MSVC: runtime estatico (/MT) para exe portavel
if (MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_executable(DLGlogger src/DLGlogger.c)
target_compile_definitions(DLGlogger PRIVATE WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
target_link_libraries(DLGlogger PRIVATE ws2_32)

# Saida sempre em bin/
set_target_properties(DLGlogger PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
)

# Instala em bin/
install(TARGETS DLGlogger RUNTIME DESTINATION bin)

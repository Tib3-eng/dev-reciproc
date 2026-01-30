cmake_minimum_required(VERSION 3.20)

# --- vcpkg toolchain ----
if (NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE "${CMAKE_SOURCE_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
endif()
set(VCPKG_TARGET_TRIPLET "x64-windows" CACHE STRING "")

project(DriveA5 C)

set(CMAKE_C_STANDARD 11)

# --- libmodbus via vcpkg ----
find_package(libmodbus CONFIG REQUIRED)

# --- app ----
add_executable(a5_cli src/a5_cli.c)
target_link_libraries(a5_cli PRIVATE modbus ws2_32)

add_executable(a5_pos_cli src/a5_pos_cli.c)
target_link_libraries(a5_pos_cli PRIVATE modbus ws2_32)

# --- copiar DLL (Debug/Release) p/ pasta do exe ----
file(GLOB_RECURSE MODBUS_DLL
  "${VCPKG_INSTALLED_DIR}/x64-windows/bin/modbus*.dll"
  "${VCPKG_INSTALLED_DIR}/x64-windows/debug/bin/modbus*.dll")
add_custom_command(TARGET a5_cli POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${MODBUS_DLL} "$<TARGET_FILE_DIR:a5_cli>"
)
add_custom_command(TARGET a5_pos_cli POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${MODBUS_DLL} "$<TARGET_FILE_DIR:a5_pos_cli>"
)

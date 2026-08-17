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

# A configuracao nao oficial do pacote pode manter Z_VCPKG_MODBUS_RELEASE
# em cache apontando para debug/lib. Fixe explicitamente cada configuracao.
set(MODBUS_LIB_RELEASE
  "${VCPKG_INSTALLED_DIR}/x64-windows/lib/modbus.lib")
set(MODBUS_LIB_DEBUG
  "${VCPKG_INSTALLED_DIR}/x64-windows/debug/lib/modbus.lib")
if (NOT EXISTS "${MODBUS_LIB_RELEASE}" OR
    NOT EXISTS "${MODBUS_LIB_DEBUG}")
  message(FATAL_ERROR "Bibliotecas libmodbus Debug/Release nao encontradas")
endif()
set_target_properties(modbus PROPERTIES
  IMPORTED_CONFIGURATIONS "Debug;Release;MinSizeRel;RelWithDebInfo"
  IMPORTED_LOCATION_DEBUG "${MODBUS_LIB_DEBUG}"
  IMPORTED_LOCATION_RELEASE "${MODBUS_LIB_RELEASE}"
  IMPORTED_LOCATION_MINSIZEREL "${MODBUS_LIB_RELEASE}"
  IMPORTED_LOCATION_RELWITHDEBINFO "${MODBUS_LIB_RELEASE}"
)

# --- app ----
add_executable(a5_cli src/a5_cli.c)
target_link_libraries(a5_cli PRIVATE modbus ws2_32)

add_executable(a5_pos_cli src/a5_pos_cli.c)
target_link_libraries(a5_pos_cli PRIVATE modbus ws2_32)

add_executable(a5_speed_logger
  src/a5_speed_logger.c
  ../EncoderCore/src/encoder_control_protocol.c
  ../EncoderCore/src/recip_encoder_controller.c
)
target_include_directories(a5_speed_logger PRIVATE ../EncoderCore/include)
target_link_libraries(a5_speed_logger PRIVATE modbus ws2_32)

add_executable(merge_logs src/merge_logs.c)

# --- copiar DLL da configuracao correta p/ pasta do exe ----
# O pacote nao informa a DLL no target importado (TARGET_FILE aponta para
# modbus.lib). Use caminhos exatos e uma unica origem por configuracao.
set(MODBUS_DLL_RELEASE
  "${VCPKG_INSTALLED_DIR}/x64-windows/bin/modbus-5.dll")
set(MODBUS_DLL_DEBUG
  "${VCPKG_INSTALLED_DIR}/x64-windows/debug/bin/modbus-5.dll")
if (NOT EXISTS "${MODBUS_DLL_RELEASE}" OR
    NOT EXISTS "${MODBUS_DLL_DEBUG}")
  message(FATAL_ERROR "DLLs libmodbus Debug/Release nao encontradas")
endif()
set(MODBUS_DLL_FOR_CONFIG
  "$<$<CONFIG:Debug>:${MODBUS_DLL_DEBUG}>$<$<NOT:$<CONFIG:Debug>>:${MODBUS_DLL_RELEASE}>")
add_custom_command(TARGET a5_cli POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${MODBUS_DLL_FOR_CONFIG}" "$<TARGET_FILE_DIR:a5_cli>"
)
add_custom_command(TARGET a5_pos_cli POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${MODBUS_DLL_FOR_CONFIG}" "$<TARGET_FILE_DIR:a5_pos_cli>"
)
add_custom_command(TARGET a5_speed_logger POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${MODBUS_DLL_FOR_CONFIG}" "$<TARGET_FILE_DIR:a5_speed_logger>"
)

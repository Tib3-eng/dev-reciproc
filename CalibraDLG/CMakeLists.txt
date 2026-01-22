cmake_minimum_required(VERSION 3.20)
project(CalibraDLG C)

set(CMAKE_C_STANDARD 11)

add_executable(CalibraDLG src/calibra_dlg.c)
target_compile_definitions(CalibraDLG PRIVATE _CRT_SECURE_NO_WARNINGS)

# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.25)
project(praesidium_humanitatis_tft_compat LANGUAGES CXX)
include(CTest)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_subdirectory(../tools/temporal-fusion-transformer temporal-fusion-transformer)

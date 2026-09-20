# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.25)
project(praesidium_humanitatis VERSION 0.2.0 LANGUAGES CXX)
include(CTest)

option(PH_BUILD_TFT "Build the MIT Temporal Fusion Transformer (requires LibTorch)" OFF)
option(PH_BUILD_PRICE_OPTIMIZATION "Build the separately licensed price optimization tool" ON)
option(PH_BUILD_EXCHANGE "Build the synthetic exchange simulation (uses the separately licensed optimizer)" OFF)

if(PH_BUILD_TFT)
  add_subdirectory(tools/temporal-fusion-transformer)
endif()
if(PH_BUILD_PRICE_OPTIMIZATION)
  add_subdirectory(tools/price-optimization)
endif()
if(PH_BUILD_EXCHANGE)
  add_subdirectory(projects/post-profit-exchange)
endif()

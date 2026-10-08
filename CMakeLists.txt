cmake_minimum_required(VERSION 3.20)
project(orderbook_sim CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-Wall -Wextra -Wpedantic)

# Matching engine + simulation, no GUI dependencies
add_library(orderbook src/OrderBook.cpp src/Simulation.cpp)
target_include_directories(orderbook PUBLIC include)

find_package(Threads REQUIRED)

add_executable(orderbook_tests tests/test_orderbook.cpp)
target_link_libraries(orderbook_tests PRIVATE orderbook)

add_executable(orderbook_bench bench/bench.cpp)
target_link_libraries(orderbook_bench PRIVATE orderbook Threads::Threads)

# GUI (raylib from Homebrew: brew install raylib)
find_package(raylib QUIET)
if(raylib_FOUND)
  add_executable(orderbook_gui src/gui/main.cpp)
  target_link_libraries(orderbook_gui PRIVATE orderbook raylib Threads::Threads)
  if(APPLE)
    target_link_libraries(orderbook_gui PRIVATE "-framework Cocoa" "-framework IOKit" "-framework OpenGL")
  endif()
else()
  message(WARNING "raylib not found, skipping the GUI. Install it with: brew install raylib")
endif()

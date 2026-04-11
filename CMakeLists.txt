cmake_minimum_required(VERSION 3.20)
project(multicore-event-driven-simulation LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(sim src/main.cpp)
target_include_directories(sim PRIVATE include)
target_compile_options(sim PRIVATE -Wall -Wextra -O2)
target_link_libraries(sim PRIVATE pthread)

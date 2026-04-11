cmake_minimum_required(VERSION 3.20)
project(celeris LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(celeris src/main.cpp)
target_include_directories(celeris PRIVATE include)
target_compile_options(celeris PRIVATE
    -Wall -Wextra
    $<$<CONFIG:Release>:-O3>
    $<$<CONFIG:Debug>:-O0 -g>
)
target_link_libraries(celeris PRIVATE pthread)

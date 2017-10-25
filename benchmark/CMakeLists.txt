cmake_minimum_required(VERSION 3.8)
project(benchmark)

set(CMAKE_CXX_STANDARD 17)

set(SOURCE_FILES benchmark.cpp)
add_executable(benchmark ${SOURCE_FILES})
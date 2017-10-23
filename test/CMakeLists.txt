cmake_minimum_required(VERSION 3.8)
project(SyncChunkedArray)

set(CMAKE_CXX_STANDARD 17)

SET( CMAKE_CXX_FLAGS  "-pthread")

set(SOURCE_FILES main.cpp ../SyncedChunkedArray.h benchmark.h)
add_executable(SyncChunkedArray ${SOURCE_FILES})
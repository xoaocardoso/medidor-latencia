cmake_minimum_required(VERSION 3.22.1)
project(medidor C)
add_library(medidor SHARED aaudio_loop.c)
target_link_libraries(medidor aaudio log)

cmake_minimum_required(VERSION 3.22.1)
project(medidor C)
add_library(medidor SHARED anc.c)
target_link_libraries(medidor aaudio log)

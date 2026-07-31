cmake_minimum_required(VERSION 3.16)
project(lowlat_stream CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-Wall -Wextra)

add_subdirectory(common)
add_subdirectory(modules/capture)
add_subdirectory(modules/encode)
add_subdirectory(app/sender)
add_subdirectory(app/receiver)
add_subdirectory(app/server)

enable_testing()
add_subdirectory(tests)
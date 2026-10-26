# Standalone D20 P0 probe; not part of the main build.
#   cmake -S tools/pie_probe -B build/pie_probe -G Ninja && cmake --build build/pie_probe
cmake_minimum_required(VERSION 3.25)
project(pie_probe LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
find_package(Qt6 REQUIRED COMPONENTS Widgets Gui)
add_executable(pie_probe pie_probe.cpp)
target_link_libraries(pie_probe PRIVATE Qt6::Widgets Qt6::Gui)

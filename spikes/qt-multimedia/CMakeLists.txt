# 0.4.1 confirmation: does Qt Multimedia build, link and decode a WAV and an MP3 (0.4.6, D10)?
# Standalone; not part of the Apex build:  cmake -S spikes/qt-multimedia -B /tmp/qtmm && cmake --build /tmp/qtmm
cmake_minimum_required(VERSION 3.25)
project(apex_qt_multimedia_spike CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Multimedia)
add_executable(qtmm_spike main.cpp)
target_link_libraries(qtmm_spike PRIVATE Qt6::Core Qt6::Multimedia)

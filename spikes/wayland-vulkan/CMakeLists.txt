cmake_minimum_required(VERSION 3.24)
project(wayland_vulkan_spike LANGUAGES CXX)

option(SPIKE_ADS "Use Qt Advanced Docking System for --layout=dock (downloads source)" OFF)
# Prefer the system loader/headers, even with a Vulkan SDK in the environment.
find_path(SPIKE_VULKAN_INCLUDE_DIR vulkan/vulkan.h PATHS /usr/include NO_DEFAULT_PATH REQUIRED)
find_library(SPIKE_VULKAN_LIBRARY vulkan PATHS /usr/lib/x86_64-linux-gnu /usr/lib64 /usr/lib NO_DEFAULT_PATH REQUIRED)
set(Vulkan_INCLUDE_DIR "${SPIKE_VULKAN_INCLUDE_DIR}" CACHE PATH "System Vulkan headers for Qt" FORCE)
find_package(Qt6 6.10 REQUIRED COMPONENTS Widgets Gui)

add_executable(wayland-vulkan main.cpp)
target_compile_features(wayland-vulkan PRIVATE cxx_std_23)
set_target_properties(wayland-vulkan PROPERTIES CXX_EXTENSIONS OFF)
target_compile_options(wayland-vulkan PRIVATE -Wall -Wextra)
target_include_directories(wayland-vulkan PRIVATE "${SPIKE_VULKAN_INCLUDE_DIR}")
target_link_libraries(wayland-vulkan PRIVATE Qt6::Widgets Qt6::Gui "${SPIKE_VULKAN_LIBRARY}")

if(SPIKE_ADS)
    include(FetchContent)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(qtads
        GIT_REPOSITORY https://github.com/githubuser0xFFFF/Qt-Advanced-Docking-System.git
        GIT_TAG 4.4.1
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(qtads)
    target_compile_definitions(wayland-vulkan PRIVATE SPIKE_ADS)
    target_link_libraries(wayland-vulkan PRIVATE qtadvanceddocking-qt6)
endif()

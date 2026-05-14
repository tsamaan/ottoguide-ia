cmake_minimum_required(VERSION 3.10)
project(otto_audio CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_BUILD_TYPE Release)

find_package(unitree_sdk2 REQUIRED
  PATHS /opt/unitree_robotics/lib/cmake/unitree_sdk2
  NO_DEFAULT_PATH
)

add_executable(otto_speak_file otto_speak_file.cpp)
target_include_directories(otto_speak_file PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(otto_speak_file unitree_sdk2)

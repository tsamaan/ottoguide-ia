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

add_executable(asr_test asr_test.cpp)
target_include_directories(asr_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(asr_test unitree_sdk2)

set(WHISPER_DIR /home/unitree/Desktop/whisper.cpp)

add_executable(otto_pipeline otto_pipeline.cpp)
target_include_directories(otto_pipeline PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${WHISPER_DIR}/include
    ${WHISPER_DIR}
    ${WHISPER_DIR}/ggml/include
)
target_link_libraries(otto_pipeline
    unitree_sdk2
    ${WHISPER_DIR}/build/src/libwhisper.so
    ${WHISPER_DIR}/build/ggml/src/libggml.so
    ${WHISPER_DIR}/build/ggml/src/libggml-base.so
    ${WHISPER_DIR}/build/ggml/src/libggml-cpu.so
    ${WHISPER_DIR}/build/ggml/src/ggml-cuda/libggml-cuda.so
    pthread
)
set_target_properties(otto_pipeline PROPERTIES
    BUILD_RPATH "${WHISPER_DIR}/build/src:${WHISPER_DIR}/build/ggml/src:${WHISPER_DIR}/build/ggml/src/ggml-cuda"
    INSTALL_RPATH "${WHISPER_DIR}/build/src:${WHISPER_DIR}/build/ggml/src:${WHISPER_DIR}/build/ggml/src/ggml-cuda"
    BUILD_WITH_INSTALL_RPATH TRUE
)

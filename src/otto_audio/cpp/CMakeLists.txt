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

# Herramienta de calibración del mic USB-C (ver ARQUITECTURA.md Fase 2) —
# standalone, no depende de unitree_sdk2/whisper, no toca otto_pipeline.
add_executable(mic_capture_test mic_capture.cpp mic_capture_test.cpp)
target_include_directories(mic_capture_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mic_capture_test asound pthread)

set(WHISPER_DIR /home/unitree/Desktop/whisper.cpp)

# Fase 0 de la mejora de VAD local (ver TODO.md, sección "Mejora de
# detección de voz"): ONNX Runtime prebuilt CPU-only oficial de Microsoft,
# descomprimido fuera del repo -- mismo criterio que WHISPER_DIR arriba.
set(ONNXRUNTIME_DIR /home/unitree/Desktop/onnxruntime-linux-aarch64-1.30.0)

# Prueba aislada: confirma que ONNX Runtime + el modelo Silero VAD
# compilan/linkean/corren en este Jetson antes de tocar otto_pipeline.
add_executable(onnx_smoke_test onnx_smoke_test.cpp)
target_include_directories(onnx_smoke_test PRIVATE ${ONNXRUNTIME_DIR}/include)
target_link_libraries(onnx_smoke_test ${ONNXRUNTIME_DIR}/lib/libonnxruntime.so)
set_target_properties(onnx_smoke_test PROPERTIES
    BUILD_RPATH "${ONNXRUNTIME_DIR}/lib"
    INSTALL_RPATH "${ONNXRUNTIME_DIR}/lib"
    BUILD_WITH_INSTALL_RPATH TRUE
)

# Fase 1: corre SileroVad (silero_vad.hpp/.cpp, la clase real que despues
# se integra a otto_pipeline en la Fase 2) sobre archivos .wav de
# referencia en test_fixtures/ -- permite calibrar sin el robot en vivo.
add_executable(vad_harness vad_harness.cpp silero_vad.cpp)
target_include_directories(vad_harness PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${ONNXRUNTIME_DIR}/include
)
target_link_libraries(vad_harness ${ONNXRUNTIME_DIR}/lib/libonnxruntime.so)
set_target_properties(vad_harness PROPERTIES
    BUILD_RPATH "${ONNXRUNTIME_DIR}/lib"
    INSTALL_RPATH "${ONNXRUNTIME_DIR}/lib"
    BUILD_WITH_INSTALL_RPATH TRUE
)

add_executable(otto_pipeline otto_pipeline.cpp mic_capture.cpp)
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
    asound
)
set_target_properties(otto_pipeline PROPERTIES
    BUILD_RPATH "${WHISPER_DIR}/build/src:${WHISPER_DIR}/build/ggml/src:${WHISPER_DIR}/build/ggml/src/ggml-cuda"
    INSTALL_RPATH "${WHISPER_DIR}/build/src:${WHISPER_DIR}/build/ggml/src:${WHISPER_DIR}/build/ggml/src/ggml-cuda"
    BUILD_WITH_INSTALL_RPATH TRUE
)

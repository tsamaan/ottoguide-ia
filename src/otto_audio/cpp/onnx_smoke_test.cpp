// Fase 0 de la mejora de VAD (ver TODO.md): prueba aislada de que ONNX
// Runtime + el modelo Silero VAD compilan, linkean y corren en este Jetson
// (L4T 35.3.1) antes de escribir la integración real en otto_pipeline.
//
// No toca audio real ni el pipeline — corre el modelo una vez sobre un
// bloque de ceros (silencio sintético) y confirma que la inferencia
// devuelve un numero sano (probabilidad de voz baja, no NaN/error).
//
// Forma de los tensores tomada del ejemplo oficial de silero-vad
// (examples/cpp/silero-vad-onnx.cpp en snakers4/silero-vad):
//   input:  float32 [1, 576]   (64 muestras de contexto + 512 de ventana, 16kHz)
//   state:  float32 [2, 1, 128]
//   sr:     int64   []         (16000)
//   output: float32 [1, 1]     (probabilidad de voz)
//   stateN: float32 [2, 1, 128] (estado actualizado)

#include <onnxruntime_cxx_api.h>
#include <iostream>
#include <vector>
#include <array>
#include <cstdint>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Uso: " << argv[0] << " <path a silero_vad.onnx>" << std::endl;
        return 1;
    }
    const char* model_path = argv[1];

    Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "onnx_smoke_test");
    Ort::SessionOptions session_options;
    session_options.SetIntraOpNumThreads(1);

    std::cout << "[SMOKE] Cargando modelo: " << model_path << std::endl;
    Ort::Session session(env, model_path, session_options);
    std::cout << "[SMOKE] Sesion creada OK." << std::endl;

    Ort::AllocatorWithDefaultOptions allocator;
    std::cout << "[SMOKE] Inputs esperados: input, state, sr -- inputs reales del modelo:" << std::endl;
    for (size_t i = 0; i < session.GetInputCount(); ++i) {
        auto name = session.GetInputNameAllocated(i, allocator);
        std::cout << "  [" << i << "] " << name.get() << std::endl;
    }
    std::cout << "[SMOKE] Outputs esperados: output, stateN -- outputs reales del modelo:" << std::endl;
    for (size_t i = 0; i < session.GetOutputCount(); ++i) {
        auto name = session.GetOutputNameAllocated(i, allocator);
        std::cout << "  [" << i << "] " << name.get() << std::endl;
    }

    // --- Una inferencia de prueba sobre silencio sintetico (todo ceros) ---
    const int context_samples = 64;
    const int window_size_samples = 512;
    const int effective_window = context_samples + window_size_samples; // 576

    std::vector<float> input_data(effective_window, 0.0f);
    std::vector<float> state_data(2 * 1 * 128, 0.0f);
    std::array<int64_t, 1> sr_data = {16000};

    std::array<int64_t, 2> input_dims = {1, effective_window};
    std::array<int64_t, 3> state_dims = {2, 1, 128};
    std::array<int64_t, 1> sr_dims = {1};

    Ort::MemoryInfo memory_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeCPU);

    Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
        memory_info, input_data.data(), input_data.size(), input_dims.data(), input_dims.size());
    Ort::Value state_tensor = Ort::Value::CreateTensor<float>(
        memory_info, state_data.data(), state_data.size(), state_dims.data(), state_dims.size());
    Ort::Value sr_tensor = Ort::Value::CreateTensor<int64_t>(
        memory_info, sr_data.data(), sr_data.size(), sr_dims.data(), sr_dims.size());

    std::vector<Ort::Value> ort_inputs;
    ort_inputs.push_back(std::move(input_tensor));
    ort_inputs.push_back(std::move(state_tensor));
    ort_inputs.push_back(std::move(sr_tensor));

    const char* input_names[] = {"input", "state", "sr"};
    const char* output_names[] = {"output", "stateN"};

    auto ort_outputs = session.Run(Ort::RunOptions{nullptr}, input_names, ort_inputs.data(),
                                    ort_inputs.size(), output_names, 2);

    float speech_prob = ort_outputs[0].GetTensorMutableData<float>()[0];
    std::cout << "[SMOKE] Inferencia sobre silencio sintetico -> probabilidad de voz: "
              << speech_prob << " (esperado: cercano a 0)" << std::endl;

    if (speech_prob < 0.0f || speech_prob > 1.0f) {
        std::cerr << "[SMOKE] FALLO: probabilidad fuera de rango [0,1] -- algo esta mal." << std::endl;
        return 1;
    }

    std::cout << "[SMOKE] OK -- ONNX Runtime + Silero VAD funcionan en este Jetson." << std::endl;
    return 0;
}

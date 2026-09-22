#include "silero_vad.hpp"
#include <onnxruntime_cxx_api.h>
#include <array>
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <vector>

// Formato de tensores y tamaños de ventana/contexto tomados del ejemplo
// oficial (snakers4/silero-vad, examples/cpp/silero-vad-onnx.cpp) --
// verificado contra el modelo real con onnx_smoke_test.cpp (Fase 0) antes
// de escribir esto.
namespace {
constexpr int kContextSamples = 64;
constexpr int kWindowSizeSamples = 512;
constexpr int kEffectiveWindow = kContextSamples + kWindowSizeSamples; // 576
constexpr int kStateSize = 2 * 1 * 128;
} // namespace

struct SileroVad::Impl {
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "silero_vad"};
    Ort::SessionOptions session_options;
    std::unique_ptr<Ort::Session> session;
    Ort::MemoryInfo memory_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeCPU);

    std::vector<float> context = std::vector<float>(kContextSamples, 0.0f);
    std::vector<float> state = std::vector<float>(kStateSize, 0.0f);

    explicit Impl(const std::string& model_path) {
        session_options.SetIntraOpNumThreads(1);
        session = std::make_unique<Ort::Session>(env, model_path.c_str(), session_options);
    }

    float predict(const int16_t* window, size_t n) {
        if ((int)n != kWindowSizeSamples) {
            throw std::runtime_error(
                "SileroVad::speech_probability: la ventana debe tener exactamente "
                "512 muestras (kWindowSizeSamples)");
        }

        std::vector<float> input(kEffectiveWindow);
        std::copy(context.begin(), context.end(), input.begin());
        for (int i = 0; i < kWindowSizeSamples; ++i)
            input[kContextSamples + i] = window[i] / 32768.0f; // normalizar a [-1,1]

        std::array<int64_t, 2> input_dims = {1, kEffectiveWindow};
        std::array<int64_t, 3> state_dims = {2, 1, 128};
        std::array<int64_t, 1> sr_dims = {1};
        std::array<int64_t, 1> sr_data = {16000};

        Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
            memory_info, input.data(), input.size(), input_dims.data(), input_dims.size());
        Ort::Value state_tensor = Ort::Value::CreateTensor<float>(
            memory_info, state.data(), state.size(), state_dims.data(), state_dims.size());
        Ort::Value sr_tensor = Ort::Value::CreateTensor<int64_t>(
            memory_info, sr_data.data(), sr_data.size(), sr_dims.data(), sr_dims.size());

        std::vector<Ort::Value> inputs;
        inputs.push_back(std::move(input_tensor));
        inputs.push_back(std::move(state_tensor));
        inputs.push_back(std::move(sr_tensor));

        const char* input_names[] = {"input", "state", "sr"};
        const char* output_names[] = {"output", "stateN"};

        auto outputs = session->Run(Ort::RunOptions{nullptr}, input_names, inputs.data(),
                                     inputs.size(), output_names, 2);

        float prob = outputs[0].GetTensorMutableData<float>()[0];
        float* new_state = outputs[1].GetTensorMutableData<float>();
        std::memcpy(state.data(), new_state, kStateSize * sizeof(float));

        // Actualizar contexto con las ultimas kContextSamples de esta ventana,
        // para que la proxima llamada tenga continuidad real (no ceros).
        for (int i = 0; i < kContextSamples; ++i)
            context[i] = window[kWindowSizeSamples - kContextSamples + i] / 32768.0f;

        return prob;
    }

    void reset() {
        std::fill(context.begin(), context.end(), 0.0f);
        std::fill(state.begin(), state.end(), 0.0f);
    }
};

SileroVad::SileroVad(const std::string& model_path) : impl_(new Impl(model_path)) {}
SileroVad::~SileroVad() = default;

float SileroVad::speech_probability(const int16_t* window, size_t n) {
    return impl_->predict(window, n);
}

void SileroVad::reset() { impl_->reset(); }

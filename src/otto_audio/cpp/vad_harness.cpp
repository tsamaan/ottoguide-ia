// Fase 1 de la mejora de VAD (ver TODO.md): corre SileroVad sobre un .wav
// de referencia completo y reporta el perfil de deteccion -- para calibrar
// sin necesitar el robot en vivo ni coordinar hablar por SSH cada vez.
#include "silero_vad.hpp"
#include "wav.hpp"
#include <iostream>
#include <cstdint>
#include <cstdlib>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Uso: " << argv[0] << " <modelo.onnx> <audio.wav> [umbral=0.5]" << std::endl;
        return 1;
    }
    std::string model_path = argv[1];
    std::string wav_path = argv[2];
    float threshold = (argc >= 4) ? std::atof(argv[3]) : 0.5f;

    int32_t sample_rate = 0;
    int8_t channels = 0;
    bool ok = false;
    auto raw = ReadWave(wav_path, &sample_rate, &channels, &ok);
    if (!ok) {
        std::cerr << "No se pudo leer " << wav_path << std::endl;
        return 1;
    }
    if (sample_rate != 16000 || channels != 1) {
        std::cerr << "Se esperaba 16kHz mono, el archivo es " << sample_rate
                  << "Hz, " << (int)channels << " canal(es)." << std::endl;
        return 1;
    }

    const int16_t* samples = reinterpret_cast<const int16_t*>(raw.data());
    size_t n_samples = raw.size() / sizeof(int16_t);

    SileroVad vad(model_path);
    const int window = vad.window_size();
    size_t n_windows = n_samples / window;
    size_t speech_windows = 0;

    std::cout << "Archivo: " << wav_path << "  (" << (n_samples / (float)sample_rate)
              << "s, " << n_windows << " ventanas de " << window << " muestras, umbral="
              << threshold << ")" << std::endl;

    for (size_t w = 0; w < n_windows; ++w) {
        float prob = vad.speech_probability(samples + w * window, window);
        bool is_speech = prob >= threshold;
        if (is_speech) speech_windows++;
        float t = (w * window) / (float)sample_rate;
        std::cout << "  t=" << t << "s  prob=" << prob << (is_speech ? "  <-- VOZ" : "")
                  << std::endl;
    }

    float pct = n_windows ? (100.0f * speech_windows / (float)n_windows) : 0.0f;
    std::cout << "Resumen: " << speech_windows << "/" << n_windows
              << " ventanas con voz (" << pct << "%)" << std::endl;
    return 0;
}

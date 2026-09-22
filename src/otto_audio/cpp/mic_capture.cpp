#include "mic_capture.hpp"
#include <alsa/asoundlib.h>
#include <iostream>

#define C_CYAN  "\033[96m"
#define C_RED   "\033[91m"
#define C_RESET "\033[0m"

// Dispositivo ALSA "pulse": pasa por el puente ALSA->PipeWire en vez de abrir
// el hardware crudo directamente. El mic USB (AB13X) ya está tomado por
// PipeWire en modo exclusivo — abrir "hw:" a secas da "Device or resource
// busy" (confirmado en el diagnóstico del 2026-09-18). Vía "pulse", PipeWire
// hace el resampleo de su tasa nativa (48kHz) a los 16kHz que pide el
// pipeline — confirmado que funciona con `arecord -D pulse -r 16000 ...`.
#define ALSA_DEVICE "pulse"

void mic_capture_thread(std::mutex& buf_mutex,
                         std::vector<int16_t>& buffer,
                         std::atomic<bool>& running,
                         int sample_rate) {
    snd_pcm_t* pcm = nullptr;
    int err = snd_pcm_open(&pcm, ALSA_DEVICE, SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
        std::cerr << C_RED "[MIC] No se pudo abrir '" ALSA_DEVICE "': "
                  << snd_strerror(err) << C_RESET << std::endl;
        return;
    }

    snd_pcm_hw_params_t* hw = nullptr;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(pcm, hw);
    snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(pcm, hw, 1);

    unsigned int rate = (unsigned int)sample_rate;
    snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, nullptr);

    snd_pcm_uframes_t period_size = sample_rate / 10; // ~100ms por período
    snd_pcm_hw_params_set_period_size_near(pcm, hw, &period_size, nullptr);

    err = snd_pcm_hw_params(pcm, hw);
    if (err < 0) {
        std::cerr << C_RED "[MIC] No se pudo configurar el hardware: "
                  << snd_strerror(err) << C_RESET << std::endl;
        snd_pcm_close(pcm);
        return;
    }

    err = snd_pcm_prepare(pcm);
    if (err < 0) {
        std::cerr << C_RED "[MIC] snd_pcm_prepare falló: " << snd_strerror(err)
                  << C_RESET << std::endl;
        snd_pcm_close(pcm);
        return;
    }

    std::cout << C_CYAN "[MIC]" C_RESET " Captura ALSA en '" ALSA_DEVICE
              << "' a " << rate << "Hz mono" << std::endl;

    std::vector<int16_t> period_buf(period_size);
    const size_t MAX_SAMPLES = (size_t)sample_rate * 30;

    while (running) {
        snd_pcm_sframes_t n = snd_pcm_readi(pcm, period_buf.data(), period_size);
        if (n == -EPIPE) {
            // Overrun: el buffer del kernel se llenó antes de que lo leyéramos.
            snd_pcm_prepare(pcm);
            continue;
        } else if (n < 0) {
            n = snd_pcm_recover(pcm, (int)n, 1);
            if (n < 0) {
                std::cerr << C_RED "[MIC] Error de captura irrecuperable: "
                          << snd_strerror((int)n) << C_RESET << std::endl;
                break;
            }
            continue;
        }
        if (n <= 0) continue;

        std::lock_guard<std::mutex> lock(buf_mutex);
        buffer.insert(buffer.end(), period_buf.begin(), period_buf.begin() + n);
        if (buffer.size() > MAX_SAMPLES)
            buffer.erase(buffer.begin(), buffer.begin() + (buffer.size() - MAX_SAMPLES));
    }

    snd_pcm_close(pcm);
}

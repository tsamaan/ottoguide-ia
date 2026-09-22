#include "mic_capture.hpp"
#include <alsa/asoundlib.h>
#include <iostream>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cctype>

#define C_CYAN   "\033[96m"
#define C_YELLOW "\033[93m"
#define C_RED    "\033[91m"
#define C_RESET  "\033[0m"

// Dispositivo ALSA "pulse": pasa por el puente ALSA->PipeWire en vez de abrir
// el hardware crudo directamente. El mic USB ya está tomado por PipeWire en
// modo exclusivo — abrir "hw:" a secas da "Device or resource busy"
// (confirmado en el diagnóstico del 2026-09-18).
#define ALSA_DEVICE "pulse"

// Nombre (en minúsculas) del mic USB-C actual, para priorizarlo si hay más
// de una fuente disponible. Si el hardware cambia, el fallback de abajo
// agarra cualquier fuente "usb" igual.
#define PREFERRED_SOURCE_HINT "ab13x"

namespace {

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

// Elige a mano la fuente de PipeWire para el mic USB-C en vez de confiar en
// el "Default Source" del sistema. Motivo: el 2026-09-22 diagnosticamos que
// otto_pipeline quedó escuchando en silencio total porque el Default Source
// apuntaba al mic interno del Jetson (`platform-sound`), no al mic USB — sin
// ningún error, el ALSA "pulse" simplemente abre lo que sea el default en
// ese momento (que depende del orden de conexión/boot, no es estable).
//
// Prioriza un nombre que contenga PREFERRED_SOURCE_HINT; si no aparece (otro
// mic USB conectado), cae a cualquier fuente "usb" que no sea `.monitor`
// (eso es una salida, no entrada) ni `platform-sound` (el mic interno roto).
std::string find_usb_source() {
    FILE* p = popen("pactl list short sources 2>/dev/null", "r");
    if (!p) return "";

    std::string preferred, fallback;
    char line[512];
    while (fgets(line, sizeof(line), p)) {
        std::string raw(line);
        std::string low = to_lower(raw);
        if (low.find(".monitor") != std::string::npos) continue;
        if (low.find("platform-sound") != std::string::npos) continue;

        size_t tab1 = raw.find('\t');
        size_t tab2 = (tab1 == std::string::npos) ? std::string::npos
                                                    : raw.find('\t', tab1 + 1);
        if (tab1 == std::string::npos || tab2 == std::string::npos) continue;
        std::string name = raw.substr(tab1 + 1, tab2 - tab1 - 1);

        if (low.find(PREFERRED_SOURCE_HINT) != std::string::npos) {
            preferred = name;
            break;
        }
        if (fallback.empty() && low.find("usb") != std::string::npos)
            fallback = name;
    }
    pclose(p);
    return preferred.empty() ? fallback : preferred;
}

} // namespace

void mic_capture_thread(std::mutex& buf_mutex,
                         std::vector<int16_t>& buffer,
                         std::atomic<bool>& running,
                         int sample_rate) {
    std::string usb_source = find_usb_source();
    if (!usb_source.empty()) {
        setenv("PULSE_SOURCE", usb_source.c_str(), 1);
        std::cout << C_CYAN "[MIC]" C_RESET " Fuente USB seleccionada: "
                  << usb_source << std::endl;
    } else {
        std::cerr << C_YELLOW "[MIC] No encontré una fuente USB por "
                     "'pactl list short sources' — cayendo al Default "
                     "Source del sistema (puede terminar siendo el mic "
                     "interno del Jetson; revisar con 'pactl info')."
                     C_RESET << std::endl;
    }

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

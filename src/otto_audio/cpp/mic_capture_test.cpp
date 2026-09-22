// Medidor de RMS en vivo para calibrar el mic USB-C antes de integrarlo a
// otto_pipeline.cpp. No toca el pipeline real — buffer y mutex propios.
#include "mic_capture.hpp"
#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <cmath>
#include <csignal>
#include <algorithm>

namespace {
std::atomic<bool> g_running{true};
void on_sigint(int) { g_running = false; }

float rms_of(const std::vector<int16_t>& s) {
    if (s.empty()) return 0.f;
    double sum = 0;
    for (auto x : s) sum += (double)x * x;
    return (float)std::sqrt(sum / s.size());
}
} // namespace

int main() {
    std::signal(SIGINT, on_sigint);

    std::mutex mtx;
    std::vector<int16_t> buffer;

    std::thread cap(mic_capture_thread, std::ref(mtx), std::ref(buffer),
                     std::ref(g_running), 16000);

    std::cout << "Hablale al mic. El pipeline real activa con RMS_THRESHOLD=2000 "
                 "-- mira si lo supera de forma sostenida.\nCtrl+C para salir.\n\n";

    const int RMS_THRESHOLD = 2000;
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        std::vector<int16_t> snapshot;
        {
            std::lock_guard<std::mutex> lock(mtx);
            snapshot = buffer;
            buffer.clear();
        }
        if (snapshot.empty()) continue;

        float rms = rms_of(snapshot);
        int bars = std::min(40, (int)(rms / 100));
        std::string bar(bars, '#');
        std::string pad(40 - bars, ' ');
        std::cout << "\rRMS: " << (int)rms << "      [" << bar << pad << "]"
                   << (rms >= RMS_THRESHOLD ? "  <-- SUPERA UMBRAL" : "                   ")
                   << std::flush;
    }

    cap.join();
    std::cout << std::endl;
    return 0;
}

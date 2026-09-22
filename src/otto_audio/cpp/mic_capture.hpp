#pragma once
#include <mutex>
#include <vector>
#include <atomic>
#include <cstdint>

// Captura de audio desde el mic USB-C (vía ALSA) y lo va agregando a
// `buffer`, recortado a un máximo de 30s — mismo contrato que el
// capture_thread() original de otto_pipeline.cpp (que leía del multicast
// UDP muerto). Corre hasta que `running` se pone en false.
void mic_capture_thread(std::mutex& buf_mutex,
                         std::vector<int16_t>& buffer,
                         std::atomic<bool>& running,
                         int sample_rate = 16000);

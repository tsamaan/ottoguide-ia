// @TASK: Pipeline completo de conversacion OttoGuide G1-EDU
// @CONTEXT: UDP mic -> Whisper STT -> Ollama LLM -> Piper TTS -> PlayStream
// @FLOW: HIBERNACION -> wake word -> ESCUCHANDO -> pregunta -> respuesta

#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <cmath>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <algorithm>
#include <ctime>
#include <cstdlib>
#include <chrono>

#include "whisper.h"
#include <unitree/common/time/time_tool.hpp>
#include <unitree/robot/channel/channel_factory.hpp>
#include <unitree/robot/g1/audio/g1_audio_client.hpp>
#include "wav.hpp"

// --- Colores ANSI -----------------------------------------------------------
#define C_RESET  "\033[0m"
#define C_BOLD   "\033[1m"
#define C_GRAY   "\033[90m"
#define C_RED    "\033[91m"
#define C_GREEN  "\033[92m"
#define C_YELLOW "\033[93m"
#define C_BLUE   "\033[94m"
#define C_CYAN   "\033[96m"
#define C_WHITE  "\033[97m"

// --- Estado -----------------------------------------------------------------
enum State { HIBERNACION, ESCUCHANDO, PROCESANDO };

// --- Barra de amplitud ------------------------------------------------------
std::string rms_bar(float rms, float threshold) {
    const int W = 20;
    int filled = std::min(W, (int)(rms / 200.0f));
    std::string bar = "[";
    for (int i = 0; i < W; ++i)
        bar += (i < filled) ? "█" : "░";
    bar += "]";
    // Color segun nivel
    std::string color = (rms < threshold) ? C_GRAY :
                        (rms < threshold * 2) ? C_YELLOW : C_GREEN;
    return color + bar + C_RESET;
}

// --- Estado como string -----------------------------------------------------
const char* estado_str(State s) {
    switch(s) {
        case HIBERNACION: return C_GRAY  "HIBERNACION" C_RESET;
        case ESCUCHANDO:  return C_GREEN "ESCUCHANDO"  C_RESET;
        case PROCESANDO:  return C_YELLOW"PROCESANDO"  C_RESET;
    }
    return "?";
}

// --- Configuracion ----------------------------------------------------------
#define MCAST_GRP      "239.168.123.161"
#define MCAST_PORT     5555
#define LOCAL_IP       "192.168.123.164"
#define SAMPLE_RATE    16000
#define CAPTURE_SECS   3
#define TIMEOUT_SECS   30
#define RMS_THRESHOLD  1300
#define CHUNK_SIZE     96000
#define SDK_VOLUME     70

#define WHISPER_MODEL  "/home/unitree/Desktop/whisper.cpp/models/ggml-large-v3-turbo.bin"
#define WHISPER_PROMPT "UADE Otto OttoGuide"
#define PIPER_BIN      "/home/unitree/piper/piper"
#define PIPER_VOICE    "/home/unitree/piper/voices/es_MX-gevy-high.onnx"
#define NET_IFACE      "eth0"

// --- Buffer compartido ------------------------------------------------------
std::mutex           buf_mutex;
std::vector<int16_t> audio_buffer;
std::atomic<bool>    running{true};

// --- AudioClient global -----------------------------------------------------
unitree::robot::g1::AudioClient* g_audio = nullptr;

// --- Frases aleatorias ------------------------------------------------------
const char* SALUDOS[] = {
    "Hola! Soy OttoMan, el robot guia de UADE. En que te puedo ayudar?",
    "Bienvenido a UADE! Soy Otto, tu guia del campus. Decime tu pregunta.",
    "Hola! Que bueno tenerte por aca. Soy Otto. Como te puedo ayudar hoy?",
    "Buenas! Soy OttoMan. Preguntame lo que quieras sobre UADE.",
    nullptr
};

const char* REPITE[] = {
    "Perdon, no te escuche bien. Podrias repetir la pregunta?",
    "No entendi bien. Podes decirme de nuevo?",
    "Disculpa, hay mucho ruido. Podrias repetirlo mas fuerte?",
    "No pude procesar eso. Repetilo por favor.",
    nullptr
};

const char* CONSULTA[] = {
    "Tenes alguna otra pregunta sobre UADE?",
    "Hay algo mas en lo que te pueda ayudar?",
    "En que otra cosa te puedo orientar?",
    "Alguna otra consulta sobre el campus?",
    nullptr
};

const char* DESPEDIDAS[] = {
    "Fue un placer ayudarte. Que disfrutes UADE!",
    "Hasta luego! Que tengas un excelente dia en el campus.",
    "Chau! Cualquier duda que tengas, ya saben donde encontrarme.",
    "Hasta pronto! Espero haberte sido de ayuda. Disfruten del campus.",
    nullptr
};

std::string frase_aleatoria(const char** lista) {
    int n = 0;
    while (lista[n]) ++n;
    if (n == 0) return "";
    return lista[rand() % n];
}

// --- Normalizar texto -------------------------------------------------------
std::string normalizar(const std::string& raw) {
    std::string s = raw;
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    struct { const char* from; const char* to; } fixes[] = {
        {"puade","uade"}, {"u ade","uade"}, {"wuade","uade"}, {"u-ade","uade"},
        {"otoman","otto"}, {"otto mann","otto"}, {"ottoman","otto"}, {"otto man","otto"},
        {nullptr, nullptr}
    };
    for (int i = 0; fixes[i].from; ++i) {
        size_t p;
        while ((p = s.find(fixes[i].from)) != std::string::npos)
            s.replace(p, strlen(fixes[i].from), fixes[i].to);
    }
    return s;
}

// --- Filtro anti-alucinaciones ----------------------------------------------
bool es_alucinacion(const std::string& t) {
    if (t.empty() || t.size() < 8) return true;

    std::string tl = t;
    std::transform(tl.begin(), tl.end(), tl.begin(), ::tolower);

    // Patrones de alucinacion conocidos
    static const char* PATRONES[] = {
        // Artefactos de Whisper
        "*", "[", "\xe2\x99\xaa", "subtitl",
        // Repeticiones del prompt
        "otto otto", "otto guide", "ottoguide", "uade otto",
        // Frases repetidas tipicas
        "empresa", "gracias","suscribite","suscribete","suscribanse","suscribete a mi canal",
        nullptr
    };

    for (int i = 0; PATRONES[i]; ++i) {
        std::string p = PATRONES[i];
        // Para patrones cortos de 1 char usar find directo
        // Para palabras: detectar repeticion (2+ veces) o match exacto
        bool es_palabra = (p.size() > 2);
        if (!es_palabra) {
            if (tl.find(p) != std::string::npos) return true;
        } else {
            // Patrones de frase exacta (otto otto, otto guide, etc.)
            bool es_frase = (p.find(' ') != std::string::npos || p == "ottoguide" || p == "subtitl");
            if (es_frase) {
                if (tl.find(p) != std::string::npos) return true;
            } else {
                // Palabras sueltas: alucinacion solo si aparecen 2+ veces
                int count = 0;
                size_t pos = 0;
                while ((pos = tl.find(p, pos)) != std::string::npos) { ++count; ++pos; }
                if (count >= 2) return true;
            }
        }
    }
    return false;
}

// --- Wake word y despedida --------------------------------------------------
bool es_wake_word(const std::string& t) {
    for (auto& w : {"hola otto","ola otto","hola oto","ola oto","hola auto"})
        if (t.find(w) != std::string::npos) return true;
    return false;
}

bool es_despedida(const std::string& t) {
    for (auto& w : {"chau","adios","hasta luego","gracias eso es todo","no mas preguntas","listo","hasta pronto"})
        if (t.find(w) != std::string::npos) return true;
    return false;
}

// --- HTTP POST para Ollama --------------------------------------------------
std::string ollama_query(const std::string& pregunta) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return "";

    struct timeval tv{30, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(11434);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) < 0) { close(sock); return ""; }

    std::string p = pregunta;
    size_t pos = 0;
    while ((pos = p.find('"', pos)) != std::string::npos) { p.replace(pos, 1, "\\\""); pos += 2; }

    std::string body = "{\"model\":\"otto\",\"prompt\":\"" + p + "\",\"stream\":false}";
    std::string req  = "POST /api/generate HTTP/1.0\r\n"
                       "Host: 127.0.0.1\r\n"
                       "Content-Type: application/json\r\n"
                       "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    send(sock, req.c_str(), req.size(), 0);

    std::string resp; char buf[4096]; int n;
    while ((n = recv(sock, buf, sizeof(buf)-1, 0)) > 0) { buf[n] = 0; resp += buf; }
    close(sock);

    std::string key = "\"response\":\"";
    size_t s = resp.find(key);
    if (s == std::string::npos) return "";
    s += key.size();
    std::string result;
    for (size_t i = s; i < resp.size(); ++i) {
        if (resp[i] == '\\' && i+1 < resp.size() && resp[i+1] == '"') { result += '"'; ++i; }
        else if (resp[i] == '\\' && i+1 < resp.size() && resp[i+1] == 'n') { result += ' '; ++i; }
        else if (resp[i] == '"') break;
        else result += resp[i];
    }
    return result;
}

// --- TTS + reproduccion -----------------------------------------------------
void otto_say(const std::string& texto) {
    std::ofstream f("/tmp/otto_pipe_text.txt");
    f << texto;
    f.close();

    system("cat /tmp/otto_pipe_text.txt | " PIPER_BIN
           " --model " PIPER_VOICE
           " --output_file /tmp/otto_pipe_raw.wav >/dev/null 2>&1");

    system("ffmpeg -y -i /tmp/otto_pipe_raw.wav -ar 16000 -ac 1 -sample_fmt s16 "
           "-af \"volume=3.0\" /tmp/otto_pipe.wav -loglevel quiet");

    int32_t sr = -1; int8_t ch = 0; bool ok = false;
    auto pcm = ReadWave("/tmp/otto_pipe.wav", &sr, &ch, &ok);
    if (!ok || sr != 16000 || ch != 1) {
        std::cerr << "[TTS] Error WAV" << std::endl; return;
    }

    std::string sid = std::to_string(unitree::common::GetCurrentTimeMillisecond());
    size_t offset = 0, total = pcm.size();
    while (offset < total) {
        size_t sz = std::min((size_t)CHUNK_SIZE, total - offset);
        std::vector<uint8_t> chunk(pcm.begin()+offset, pcm.begin()+offset+sz);
        g_audio->PlayStream("otto", sid, chunk);
        offset += sz;
        unitree::common::Sleep(1);
    }
    double dur = (double)total / (16000.0 * 2.0);
    unitree::common::Sleep((int)dur + 2);
    g_audio->PlayStop("otto");
    // Limpiar buffer para evitar que la voz de Otto se transcriba como pregunta
    {
        std::lock_guard<std::mutex> lock(buf_mutex);
        audio_buffer.clear();
    }
}

// --- Thread captura UDP multicast -------------------------------------------
void capture_thread() {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct timeval tv{1, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(MCAST_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    bind(sock, (sockaddr*)&addr, sizeof(addr));

    ip_mreq mreq{};
    inet_pton(AF_INET, MCAST_GRP, &mreq.imr_multiaddr);
    inet_pton(AF_INET, LOCAL_IP,  &mreq.imr_interface);
    setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

    std::cout << C_CYAN "[MIC]" C_RESET " Captura UDP en " C_BOLD << MCAST_GRP << ":" << MCAST_PORT << C_RESET << std::endl;
    
    while (running) {
        char buf[65535];
        ssize_t n = recvfrom(sock, buf, sizeof(buf), 0, nullptr, nullptr);
        if (n <= 0) continue;
        std::lock_guard<std::mutex> lock(buf_mutex);
        const int16_t* ptr = reinterpret_cast<const int16_t*>(buf);
        audio_buffer.insert(audio_buffer.end(), ptr, ptr + n/2);
        const size_t MAX = SAMPLE_RATE * 30;
        if (audio_buffer.size() > MAX)
            audio_buffer.erase(audio_buffer.begin(),
                               audio_buffer.begin() + (audio_buffer.size() - MAX));
    }
    close(sock);
}

// --- RMS --------------------------------------------------------------------
float calcular_rms(const std::vector<int16_t>& s) {
    if (s.empty()) return 0.0f;
    double sum = 0;
    for (auto x : s) sum += (double)x * x;
    return (float)std::sqrt(sum / s.size());
}

// --- Transcribir con Whisper ------------------------------------------------
std::string transcribir(whisper_context* ctx, const std::vector<int16_t>& pcm_i16) {
    std::vector<float> pcm_f32(pcm_i16.size());
    for (size_t i = 0; i < pcm_i16.size(); ++i)
        pcm_f32[i] = pcm_i16[i] / 32768.0f;

    whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_BEAM_SEARCH);
    params.language         = "es";
    params.print_progress   = false;
    params.print_realtime   = false;
    params.print_timestamps = false;
    params.no_context       = true;
    params.initial_prompt   = WHISPER_PROMPT;
    params.n_threads        = 4;

    auto t0 = std::chrono::steady_clock::now();
    if (whisper_full(ctx, params, pcm_f32.data(), (int)pcm_f32.size()) != 0)
        return "";

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    std::cout << "\033[96m[STT]\033[0m Whisper: " << ms << "ms" << std::endl;
    
    std::string result;
    int nseg = whisper_full_n_segments(ctx);
    for (int i = 0; i < nseg; ++i)
        result += whisper_full_get_segment_text(ctx, i);
    while (!result.empty() && (result[0]==' ' || result[0]=='\n'))
        result.erase(0, 1);
    return result;
}

// --- Tomar chunk del buffer -------------------------------------------------
std::vector<int16_t> tomar_audio(size_t segundos) {
    size_t n = SAMPLE_RATE * segundos;
    std::lock_guard<std::mutex> lock(buf_mutex);
    if (audio_buffer.size() < n) return {};
    std::vector<int16_t> chunk(audio_buffer.end()-n, audio_buffer.end());
    audio_buffer.clear();
    return chunk;
}

// --- Main + state machine ---------------------------------------------------
int main(int argc, char const *argv[]) {
    srand(time(nullptr));
    std::cout << "[OTTO] Iniciando OttoGuide pipeline..." << std::endl;

    unitree::robot::ChannelFactory::Instance()->Init(0, NET_IFACE);

    unitree::robot::g1::AudioClient audio_client;
    audio_client.SetTimeout(10.0f);
    audio_client.Init();
    audio_client.SetVolume(SDK_VOLUME);
    g_audio = &audio_client;

    uint8_t vol = 0;
    if (audio_client.GetVolume(vol) != 0) {
        std::cerr << "[ERROR] No se pudo conectar al AudioClient." << std::endl;
        return 1;
    }
    std::cout << "[OK] AudioClient conectado. Volumen: " << (int)vol << std::endl;

    std::cout << "[WHISPER] Cargando modelo..." << std::endl;
    whisper_context_params cparams = whisper_context_default_params();
    cparams.use_gpu = true;
    whisper_context* wctx = whisper_init_from_file_with_params(WHISPER_MODEL, cparams);
    if (!wctx) {
        std::cerr << "[ERROR] No se pudo cargar el modelo Whisper." << std::endl;
        return 1;
    }
    std::cout << "[OK] Whisper cargado en GPU." << std::endl;

    std::thread cap(capture_thread);
    sleep(CAPTURE_SECS);

    State estado = HIBERNACION;
    time_t ultimo_habla = time(nullptr);
    std::cout << C_GREEN C_BOLD "\n╔════════════════════════════════════╗"
          << "\n║   OttoGuide listo en HIBERNACION   ║"
          << "\n║   Decí 'Hola Otto' para activar    ║"
          << "\n╚════════════════════════════════════╝\n" C_RESET << std::endl;
          
    while (running) {
        size_t secs = (estado == ESCUCHANDO) ? 5 : CAPTURE_SECS;
        auto chunk = tomar_audio(secs);
        if (chunk.empty()) { sleep(1); continue; }

        float rms = calcular_rms(chunk);

        // Sin voz suficiente
        if (rms < RMS_THRESHOLD) {
            if (estado == ESCUCHANDO &&
                difftime(time(nullptr), ultimo_habla) > TIMEOUT_SECS) {
                std::cout << "[OTTO] Timeout -> HIBERNACION" << std::endl;
                otto_say(frase_aleatoria(DESPEDIDAS));
                estado = HIBERNACION;
            }
            sleep(1);
            continue;
        }

        std::cout << C_CYAN "[MIC]" C_RESET " " << rms_bar(rms, RMS_THRESHOLD)
            << " RMS:" << C_BOLD << (int)rms << C_RESET
            << "  Estado:" << estado_str(estado) << std::endl;
        std::cout << C_CYAN "[STT]" C_RESET " Transcribiendo..." << std::endl;

        std::string texto = transcribir(wctx, chunk);
        if (texto.empty()) continue;

        // Filtrar alucinaciones
        if (es_alucinacion(texto)) {
        std::cout << C_GRAY "[FILTRO] Alucinacion descartada: \"" << texto << "\"" << C_RESET << std::endl;            
        continue;
        }

        std::cout << C_WHITE "[STT]" C_RESET " \"" << C_BOLD << texto << C_RESET << "\"" << std::endl;
        ultimo_habla = time(nullptr);
        std::string t = normalizar(texto);

        // --- HIBERNACION ---
        if (estado == HIBERNACION) {
            if (es_wake_word(t)) {
                std::cout << "[OTTO] Wake word -> ESCUCHANDO" << std::endl;
                estado = ESCUCHANDO;
                otto_say(frase_aleatoria(SALUDOS));
            }
        }
        // --- ESCUCHANDO ---
        else if (estado == ESCUCHANDO) {
            if (es_despedida(t)) {
                std::cout << C_GRAY "\n[OTTO]" C_RESET " Despedida → " C_GRAY C_BOLD "HIBERNACION" C_RESET "\n" << std::endl;                
                otto_say(frase_aleatoria(DESPEDIDAS));
                estado = HIBERNACION;
                continue;
            }
            // Pregunta muy corta o no clara
            if (texto.size() < 5) {
                otto_say(frase_aleatoria(REPITE));
                continue;
            }
            std::cout << C_YELLOW "[LLM]" C_RESET " Consultando Ollama: \"" << texto << "\"" << std::endl;            
            estado = PROCESANDO;
            std::string respuesta = ollama_query(texto);
            if (respuesta.empty()) {
                otto_say(frase_aleatoria(REPITE));
            } else {
                std::cout << C_YELLOW "[LLM]" C_RESET " Respuesta: \"" << C_BOLD << respuesta << C_RESET << "\"" << std::endl;                
                otto_say(respuesta);
                otto_say(frase_aleatoria(CONSULTA));
            }
            estado = ESCUCHANDO;
        }
    }

    running = false;
    cap.join();
    whisper_free(wctx);
    std::cout << "[OTTO] Pipeline detenido." << std::endl;
    return 0;
}

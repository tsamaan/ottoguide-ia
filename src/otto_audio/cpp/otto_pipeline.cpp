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
#include <memory>
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
#include "mic_capture.hpp"
#include "vad.hpp"
#include "rms_vad.hpp"
#include "silero_vad.hpp"

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

// Indicador visual de estado en terminal (sobreescribe la linea actual)
void print_indicador(State s) {
    switch(s) {
        case HIBERNACION:
            std::cout << C_GRAY "\r[◯] HIBERNACION   esperando 'Hola Otto'...        " C_RESET << std::flush;
            break;
        case ESCUCHANDO:
            std::cout << C_GREEN "\r[●] ESCUCHANDO    habla ahora...                  " C_RESET << std::flush;
            break;
        case PROCESANDO:
            std::cout << C_YELLOW "\r[⟳] PROCESANDO    espera un momento...            " C_RESET << std::flush;
            break;
    }
}

// --- Configuracion ----------------------------------------------------------
#define MCAST_GRP      "239.168.123.161"
#define MCAST_PORT     5555
#define LOCAL_IP       "192.168.123.164"
#define SAMPLE_RATE    16000
#define CAPTURE_SECS   3
// TIMEOUT_SECS eliminado el 2026-09-22: ya no hace falta, ESCUCHANDO ahora
// procesa una sola pregunta por "Hola Otto" y siempre vuelve a HIBERNACION
// (ver el estado ESCUCHANDO en main()).
// Bajado de 2000 a 800 el 2026-09-22: con el mic USB-C nuevo (AB13X), el
// RMS_THRESHOLD se compara contra el promedio de TODO un bloque de
// CAPTURE_SECS (3s) — un "Hola Otto" (<1s) diluido en 3s de silencio no
// llegaba a 2000 aunque el pico de voz sí lo superaba (medido: silencio
// real ~1-40, picos de voz 190-3300 según la ventana). 800 deja margen
// amplio sobre el ruido de fondo real sin exigir que la mitad del bloque
// entero sea voz sostenida.
#define RMS_THRESHOLD  800
#define CHUNK_SIZE     96000

// Fase 2 de la mejora de VAD (ver TODO.md): modelo local de Silero VAD.
// Si no carga (librería/modelo faltante), el pipeline cae a RmsVad
// (RMS_THRESHOLD de arriba) en vez de romperse -- ver construcción del
// VAD en main().
#define SILERO_MODEL_PATH "/home/unitree/Desktop/silero_vad/silero_vad.onnx"
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

// Cierre tras responder -- reescritas el 2026-09-22: ya NO sigue
// escuchando después de esto (ver ESCUCHANDO en main()), así que no
// pueden sonar como si esperaran una respuesta inmediata sin wake word.
const char* CONSULTA[] = {
    "Si tenes otra consulta, decime Hola Otto de nuevo.",
    "Para otra pregunta, volve a decirme Hola Otto.",
    "Cualquier otra duda, aca estoy, decime Hola Otto.",
    "Si necesitas algo mas, llamame con Hola Otto.",
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
        {"puade","uade"}, {"u ade","uade"}, {"wuade","uade"}, {"u-ade","uade"}, {"Guay","uade"}, 
        {"uate", "uade"},{"uage", "uade"}, {"uadi", "uade"}, {"u-a-d-e", "uade"}, {"uáde", "uade"}, {"wuade", "uade"}, {"uh ade", "uade"},
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
        "empresa", "gracias","suscribite","suscribete","suscribanse","suscribete a mi canal","eheh", "eh eh", 
        "ehe", "mmm", "hmm", "ugh",
        "radio-canada", "sous-titrage", "ende", "udrundr",
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
    for (auto& w : {"chau","chao","adios","hasta luego","gracias eso es todo","no mas preguntas","listo","hasta pronto", "chau otto","chao otto","adios otto","hasta luego otto", "gracias otto"})
        if (t.find(w) != std::string::npos) return true;
    return false;
}

// --- Detectar frases de salida/cierre (alta prioridad, antes de filtros) ----
// @TASK: Interceptar despedidas cortas (ej: "Chao") ANTES de es_alucinacion()
// @INPUT: texto raw de Whisper (puede ser muy corto)
// @OUTPUT: true si usuario quiere terminar la sesión
bool es_frase_salida(const std::string& texto) {
    if (texto.empty()) return false;

    // STEP 1: Normalizar a minúsculas
    std::string tl = texto;
    std::transform(tl.begin(), tl.end(), tl.begin(), ::tolower);

    // STEP 2: Palabras clave de salida (cobertura alta, ambigüedad baja)
    static const char* SALIDAS[] = {
        // Despedidas muy cortas (lo que Whisper captura de hablantes rápidos)
        "chao", "chau", "adios", "adiós", "bye",
        // Despedidas formales
        "hasta luego", "hasta pronto", "nos vemos", "hasta la vista",
        // Con nombre del robot
        "gracias otto", "gracias ottoman", "listo otto", "ya otto",
        // Variantes comunes
        "listo", "ya", "chao otto", "chau otto", "adios otto",
        nullptr
    };

    // STEP 3: Búsqueda directa con early exit
    for (int i = 0; SALIDAS[i]; ++i) {
        if (tl.find(SALIDAS[i]) != std::string::npos)
            return true;
    }

    return false;
}

// Verificar que el texto es una consulta valida antes de mandar a Ollama
// @INPUT: texto transcripto por Whisper
// @OUTPUT: true si es valido para consultar, false si hay que pedir que repita
bool es_texto_valido(const std::string& texto) {
    if (texto.size() < 8) return false;

    // Contar palabras
    int palabras = 0;
    bool en_palabra = false;
    for (char c : texto) {
        if (c == ' ' || c == '\n' || c == '.' || c == ',') { en_palabra = false; }
        else if (!en_palabra) { en_palabra = true; ++palabras; }
    }
    if (palabras < 2) return false;

    // Detectar patrones de otros idiomas comunes
    std::string tl = texto;
    std::transform(tl.begin(), tl.end(), tl.begin(), ::tolower);
    static const char* OTROS_IDIOMAS[] = {
        "thank you", "you are", "what is", "how are", "please",
        "hello", "the ", "olha", "voce", "nao ", "isso",
        nullptr
    };
    for (int i = 0; OTROS_IDIOMAS[i]; ++i)
        if (tl.find(OTROS_IDIOMAS[i]) != std::string::npos) return false;

    return true;
}

// --- Validar si texto tiene intención clara y estructura semántica mínima -----
// @TASK: Aceptar consultas coloquiales/imperativas CON referente (no fragmentos)
// @INPUT: texto transcripto por Whisper (con o sin puntuación)
// @OUTPUT: true si tiene intención clara + estructura mínima, false si es ruido/incompleto
bool es_consulta_coherente(const std::string& texto) {
    // STEP 1: Filtro base de longitud
    if (texto.size() <= 4) return false;

    // STEP 2: Contar palabras (mínimo 2)
    int palabras = 0;
    bool en_palabra = false;
    for (char c : texto) {
        if (c == ' ' || c == '\n' || c == '.' || c == ',' || c == '?' || c == '!') {
            en_palabra = false;
        } else if (!en_palabra) {
            en_palabra = true;
            ++palabras;
        }
    }
    if (palabras < 2) return false;

    // STEP 3: Normalizar para búsqueda
    std::string tl = texto;
    std::transform(tl.begin(), tl.end(), tl.begin(), ::tolower);

    // STEP 4: Vocabularios para validación estructural
    static const char* INTERROGATIVOS[] = {
        "cuánto", "cuanto", "cuantos", "cuántos", "qué", "que", "cuál", "cual", "quién",
        "cómo", "como", "dónde", "donde", nullptr
    };

    static const char* VERBOS_CONSULTA[] = {
        "tiene", "tienen", "tengo", "hay", "esta", "está", "estan", "están",
        "queda", "quedan", "es", "son", "puedo", "podes", "podés", "sabe", "sabés",
        "encuentra", "sirve", nullptr
    };

    static const char* IMPERATIVOS[] = {
        "contame", "decime", "explicame", "ayudame", "mostrame", "guiame", "guíame",
        "hablame", "háblame", "dame", nullptr
    };

    static const char* VERBOS_DESEO[] = {
        "quiero", "busco", "saber", "conocer", "necesito", "quisiera", "necesitaba", nullptr
    };

    // STEP 5: Sustantivos comunes y contexto UADE (para validar estructura mínima)
    static const char* SUSTANTIVOS_VALIDOS[] = {
        // Contexto UADE específico
        "uade", "aula", "aulas", "piso", "pisos", "biblioteca", "carrera", "carreras",
        "materia", "materias", "inscripcion", "inscripción", "bedelia", "bedelía",
        "gimnasio", "comedor", "ingreso", "facultad", "laboratorio", "lab", "uadeone",
        "horario", "horarios", "profesor", "profesores", "campus", "sede",
        // Sustantivos generales contextuales
        "camino", "ruta", "lugar", "sitio", "cosa", "gente", "persona", "estudiante",
        "clase", "aula", "piso", "información", "info", "data", nullptr
    };

    // STEP 6: Detectar tipo de consulta y validar estructura
    bool tiene_interrogativo = false;
    bool tiene_verbo_consulta = false;
    bool tiene_imperativo = false;
    bool tiene_verbo_deseo = false;
    bool tiene_sustantivo = false;

    for (int i = 0; INTERROGATIVOS[i]; ++i) {
        if (tl.find(INTERROGATIVOS[i]) != std::string::npos) {
            tiene_interrogativo = true;
            break;
        }
    }

    for (int i = 0; VERBOS_CONSULTA[i]; ++i) {
        if (tl.find(VERBOS_CONSULTA[i]) != std::string::npos) {
            tiene_verbo_consulta = true;
            break;
        }
    }

    for (int i = 0; IMPERATIVOS[i]; ++i) {
        if (tl.find(IMPERATIVOS[i]) != std::string::npos) {
            tiene_imperativo = true;
            break;
        }
    }

    for (int i = 0; VERBOS_DESEO[i]; ++i) {
        if (tl.find(VERBOS_DESEO[i]) != std::string::npos) {
            tiene_verbo_deseo = true;
            break;
        }
    }

    for (int i = 0; SUSTANTIVOS_VALIDOS[i]; ++i) {
        if (tl.find(SUSTANTIVOS_VALIDOS[i]) != std::string::npos) {
            tiene_sustantivo = true;
            break;
        }
    }

    // STEP 7: Aplicar reglas de estructura mínima
    // Regla A: Interrogativo SOLO sin sustantivo/contexto = RECHAZAR
    // (ej: "cuántos tiene?" sin referente)
    if (tiene_interrogativo && tiene_verbo_consulta) {
        if (!tiene_sustantivo && palabras < 4) return false;
    }

    // Regla B: Imperativo SOLO sin contexto = RECHAZAR
    // (ej: "Contame" sin tema)
    if (tiene_imperativo && !tiene_sustantivo && palabras < 3) return false;

    // Regla C: Aceptar si tiene intención clara + estructura mínima
    if (tiene_imperativo && (tiene_sustantivo || palabras >= 3)) return true;
    if (tiene_verbo_deseo && (tiene_sustantivo || palabras >= 3)) return true;
    if (tiene_interrogativo && (tiene_sustantivo || palabras >= 4)) return true;
    if (tiene_verbo_consulta && (tiene_sustantivo || palabras >= 3)) return true;

    // Regla D: Signos de puntuación NO son suficientes por sí solos
    // @SECURITY: Evitar aceptar "¿Cuántos pisos?" sin referente (incompleto)
    // Las despedidas (¿Chao?) ya fueron filtradas en FILTER 0, así que aquí
    // solo llegan consultas. Exigir estructura mínima incluso con signos.
    if (texto.find('?') != std::string::npos || texto.find("¿") != std::string::npos) {
        // Tiene signos ¿?, pero ¿tiene sustantivo o contexto?
        // Requiere: sustantivo O ≥4 palabras (para compensar falta de verbos explícitos)
        if (tiene_sustantivo || palabras >= 4) return true;
        // Tiene signos pero es muy corto sin sustantivo → RECHAZA
        return false;
    }

    return false;
}

// --- Rechazo contextualizado según tipo de error -----------------------------
// @TASK: Elegir respuesta amigable y contextuada cuando falla validación semántica
// @INPUT: texto que no pasó validación
// @OUTPUT: frase de rechazo personalizada (no solo genérica)
std::string seleccionar_rechazo_contextual(const std::string& texto) {
    // Si es muy corto: pedir que amplie
    if (texto.size() <= 5) {
        const char* RECHAZOS_CORTO[] = {
            "Eso que escuché fue muy cortito. ¿Me repetís lo que necesitás?",
            "Casi no te escucho bien. ¿Podés hablar un poquito más fuerte?",
            nullptr
        };
        return frase_aleatoria(RECHAZOS_CORTO);
    }

    // Si es largo pero sin sentido: ofrecer ayuda contextual
    const char* RECHAZOS_RUIDO[] = {
        "No llegué a entender tu consulta. ¿Podés preguntarme sobre aulas, carreras, horarios o servicios de UADE?",
        "Perdón, no capté bien. ¿Me hacés la pregunta de nuevo? Podés preguntar lo que sea sobre el campus.",
        nullptr
    };
    return frase_aleatoria(RECHAZOS_RUIDO);
}

// --- HTTP POST para Ollama --------------------------------------------------
std::string ollama_query(const std::string& pregunta) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return "";

    struct timeval tv{60, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(11434);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) < 0) { close(sock); return ""; }

    std::string p = pregunta;
    size_t pos = 0;
    while ((pos = p.find('"', pos)) != std::string::npos) { p.replace(pos, 1, "\\\""); pos += 2; }

    std::string body = "{\"model\":\"otto-llama3\",\"prompt\":\"" + p + "\",\"stream\":false,\"think\":false}";
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
    double dur = (double)total / (16000.0 * 2.0);
    auto t_start = std::chrono::steady_clock::now();

    while (offset < total) {
        size_t sz = std::min((size_t)CHUNK_SIZE, total - offset);
        std::vector<uint8_t> chunk(pcm.begin()+offset, pcm.begin()+offset+sz);
        g_audio->PlayStream("otto", sid, chunk);
        offset += sz;
        if (offset < total) unitree::common::Sleep(1);
    }

    // Esperar solo el tiempo restante hasta que termine el audio
    auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t_start).count();
    double remaining = dur + 0.3 - elapsed;
    if (remaining > 0) usleep((int)(remaining * 1000000));
    g_audio->PlayStop("otto");
    // Limpiar buffer para evitar que la voz de Otto se transcriba como pregunta
    {
        std::lock_guard<std::mutex> lock(buf_mutex);
        audio_buffer.clear();
    }
}

// Indicador sonoro: tono 880Hz 250ms cuando Otto activa ESCUCHANDO
void otto_beep() {
    const int DUR_SAMPLES = SAMPLE_RATE / 4;        // 250ms = 4000 muestras
    const float FREQ      = 880.0f;
    const float FADE      = SAMPLE_RATE * 0.02f;    // 20ms fade in/out

    std::vector<uint8_t> pcm(DUR_SAMPLES * 2);
    for (int i = 0; i < DUR_SAMPLES; ++i) {
        float env = 1.0f;
        if (i < FADE)                    env = i / FADE;
        else if (i > DUR_SAMPLES - FADE) env = (DUR_SAMPLES - i) / FADE;

        float t      = (float)i / SAMPLE_RATE;
        int16_t s    = (int16_t)(32000 * env * std::sin(2.0f * M_PI * FREQ * t));
        pcm[i*2]     = s & 0xFF;
        pcm[i*2 + 1] = (s >> 8) & 0xFF;
    }

    g_audio->SetVolume(100);
    std::string sid = std::to_string(unitree::common::GetCurrentTimeMillisecond());
    g_audio->PlayStream("otto", sid, pcm);
    usleep(400000);
    g_audio->PlayStop("otto");
    g_audio->SetVolume(SDK_VOLUME);

    // Limpiar buffer para que el beep no se transcriba como voz
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
    // Normalizar amplitud para mejorar precision de Whisper
    float max_val = 1.0f;
    for (auto s : pcm_i16)
        max_val = std::max(max_val, std::abs((float)s));
    float gain = (max_val > 500.0f) ? (20000.0f / max_val) : 1.0f;

    std::vector<float> pcm_f32(pcm_i16.size());
    for (size_t i = 0; i < pcm_i16.size(); ++i)
        pcm_f32[i] = std::min(1.0f, std::max(-1.0f, (pcm_i16[i] * gain) / 32768.0f));

    whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_BEAM_SEARCH);
    params.language         = "es";
    params.print_progress   = false;
    params.print_realtime   = false;
    params.print_timestamps = false;
    params.no_context       = true;
    params.initial_prompt   = WHISPER_PROMPT;
    params.n_threads        = 4;
    params.beam_search.beam_size = 3;
    params.no_speech_thold  = 0.4f;
    params.temperature      = 0.0f;

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

// VAD: capturar utterance completa (espera voz -> graba -> corta en silencio)
// @INPUT: vad = detector a usar (Silero o RMS de respaldo, ver main());
//         ms_voz_minima = cuanta voz sostenida hace falta para confirmar
//         que "esta hablando" (evita que un ruido aislado dispare esto);
//         ms_silencio = ms de silencio para cortar; ms_max = limite total.
// @OUTPUT: vector con PCM de la utterance, o vacio si no hubo voz real
// en el tiempo limite.
//
// Generico sobre el tamaño de ventana: cada VAD pide el suyo
// (vad.window_size() -- 512 muestras/32ms para Silero, 2400/150ms para el
// RMS de respaldo), tomar_utterance() no asume nada fijo.
std::vector<int16_t> tomar_utterance(VoiceActivityDetector& vad, int ms_voz_minima = 300,
                                      int ms_silencio = 700, int ms_max = 8000) {
    const int WINDOW_SAMPLES = vad.window_size();
    const int MS_PER_WINDOW  = WINDOW_SAMPLES * 1000 / SAMPLE_RATE;
    const int VOICE_WINDOWS   = std::max(1, ms_voz_minima / MS_PER_WINDOW);
    const int SILENCE_WINDOWS = std::max(1, ms_silencio / MS_PER_WINDOW);
    const int MAX_WINDOWS     = std::max(1, ms_max / MS_PER_WINDOW);

    std::vector<int16_t> utterance;
    int silence_count = 0;
    int voice_count   = 0;
    bool hablando     = false;

    for (int w = 0; w < MAX_WINDOWS; ++w) {
        usleep(MS_PER_WINDOW * 1000);

        std::vector<int16_t> window;
        {
            std::lock_guard<std::mutex> lock(buf_mutex);
            if (audio_buffer.size() >= (size_t)WINDOW_SAMPLES) {
                window.assign(audio_buffer.begin(), audio_buffer.begin() + WINDOW_SAMPLES);
                audio_buffer.erase(audio_buffer.begin(), audio_buffer.begin() + WINDOW_SAMPLES);
            }
        }
        if (window.empty()) continue;

        bool es_voz = vad.is_speech(window.data(), window.size());

        if (es_voz) {
            voice_count++;
            silence_count = 0;
            utterance.insert(utterance.end(), window.begin(), window.end());
            if (voice_count >= VOICE_WINDOWS) hablando = true;
        } else if (hablando) {
            silence_count++;
            utterance.insert(utterance.end(), window.begin(), window.end());
            if (silence_count >= SILENCE_WINDOWS) break;
        } else {
            voice_count = 0; // resetear si el spike fue aislado
        }
    }

    if (voice_count < VOICE_WINDOWS) return {};
    return utterance;
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

    // VAD: Silero (local, ONNX Runtime) por defecto -- distingue voz real
    // de ruido, a diferencia del umbral de RMS que reemplaza (ver
    // TODO.md, "Mejora de detección de voz"). Si no carga (librería o
    // modelo faltante/corrupto), cae a RmsVad -- el robot sigue andando
    // degradado en vez de no arrancar.
    std::unique_ptr<VoiceActivityDetector> vad;
    try {
        vad = std::make_unique<SileroVad>(SILERO_MODEL_PATH, 0.5f);
        std::cout << C_GREEN "[VAD] Silero VAD cargado OK." C_RESET << std::endl;
    } catch (const std::exception& e) {
        std::cerr << C_YELLOW "[VAD] No se pudo cargar Silero VAD (" << e.what()
                   << ") -- usando RMS de respaldo (umbral " << RMS_THRESHOLD << ")."
                   C_RESET << std::endl;
        vad = std::make_unique<RmsVad>((float)RMS_THRESHOLD);
    }

    // Mic USB-C (AB13X) via ALSA, reemplaza el capture_thread() de multicast
    // UDP (239.168.123.161:5555 sin publicar nada desde que se rompio el
    // mic interno). capture_thread() queda definido mas arriba sin usarse,
    // por si hay que volver atras rapido.
    std::thread cap(mic_capture_thread, std::ref(buf_mutex), std::ref(audio_buffer),
                     std::ref(running), SAMPLE_RATE);
    sleep(CAPTURE_SECS);

    State estado = HIBERNACION;
    std::cout << C_GREEN C_BOLD "\n╔════════════════════════════════════╗"
          << "\n║   OttoGuide listo en HIBERNACION   ║"
          << "\n║   Decí 'Hola Otto' para activar    ║"
          << "\n╚════════════════════════════════════╝\n" C_RESET << std::endl;
          
    while (running) {

        // --- HIBERNACION: VAD por ventanas cortas, igual que ESCUCHANDO ---
        // Antes usaba tomar_audio(CAPTURE_SECS): un bloque ciego de 3s fijo,
        // promediado entero contra RMS_THRESHOLD. Eso diluia un "Hola Otto"
        // corto si el resto del bloque era silencio, y si alguien hablaba
        // antes en la misma ventana, esa voz ajena se colaba en el chunk
        // (bug reportado 2026-09-22: "tuve que repetirlo varias veces").
        // tomar_utterance() ya resuelve esto en ESCUCHANDO -- arranca a
        // grabar recien cuando detecta voz sostenida (300ms) y corta en
        // silencio real, sin depender de donde caiga un bloque fijo.
        if (estado == HIBERNACION) {
            print_indicador(HIBERNACION);

            auto chunk = tomar_utterance(*vad, 300, 500);
            if (chunk.empty()) continue;

            float rms = calcular_rms(chunk);

            std::cout << "\n" << C_CYAN "[MIC]" C_RESET " " << rms_bar(rms, RMS_THRESHOLD)
                      << " RMS:" << C_BOLD << (int)rms << C_RESET << std::endl;
            std::cout << C_CYAN "[STT]" C_RESET " Transcribiendo..." << std::endl;

            std::string texto = transcribir(wctx, chunk);
            if (texto.empty()) continue;
            if (es_alucinacion(texto)) {
                std::cout << C_GRAY "[FILTRO] \"" << texto << "\"" << C_RESET << std::endl;
                continue;
            }

            std::cout << C_WHITE "[STT]" C_RESET " \"" << C_BOLD << texto << C_RESET << "\"" << std::endl;
            std::string t = normalizar(texto);

            if (es_wake_word(t)) {
                std::cout << C_GREEN C_BOLD "\n[OTTO] Wake word detectada -> ESCUCHANDO\n" C_RESET << std::endl;
                estado = ESCUCHANDO;
                otto_say(frase_aleatoria(SALUDOS));
                otto_beep();
            }
        }

        // --- ESCUCHANDO: VAD para capturar utterance completa ---
        // ESCUCHANDO: UNA pregunta por wake word, sin sesion extendida.
        // Rediseñado el 2026-09-22: antes se quedaba escuchando indefinido
        // (hasta 30s de timeout) esperando una posible segunda pregunta sin
        // repetir "Hola Otto" -- patron tipo Alexa/Siri, pensado para un
        // asistente hogareño con un solo usuario. Ottoman es un robot
        // publico de campus: en esa ventana extendida, cualquier voz de
        // CUALQUIER persona que pasara cerca se trataba como continuacion
        // de la conversacion -- ambiguo y la causa mas probable de
        // respuestas "raras" (le contestaba a ruido/charla ajena, no a
        // quien lo desperto). Ahora: captura una utterance, la procesa,
        // y SIEMPRE vuelve a HIBERNACION -- para preguntar de nuevo hay
        // que decir "Hola Otto" otra vez. Mas simple y sin ambigüedad de
        // a quien le esta escuchando.
        else if (estado == ESCUCHANDO) {
            print_indicador(ESCUCHANDO);

            auto chunk = tomar_utterance(*vad, 300, 500);
            if (chunk.empty()) {
                // No dijo nada -> vuelve a dormir, sin drama.
                estado = HIBERNACION;
                continue;
            }

            float rms = calcular_rms(chunk);
            std::cout << "\n" << C_CYAN "[MIC]" C_RESET " " << rms_bar(rms, RMS_THRESHOLD)
                      << " RMS:" << C_BOLD << (int)rms << C_RESET << std::endl;
            std::cout << C_CYAN "[STT]" C_RESET " Transcribiendo..." << std::endl;

            auto t_stt_start = std::chrono::steady_clock::now();
            std::string texto = transcribir(wctx, chunk);
            double stt_secs = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - t_stt_start).count();
            std::cout << C_GRAY "[TIEMPO] STT: " << stt_secs << "s" C_RESET << std::endl;
            if (texto.empty()) { estado = HIBERNACION; continue; }

            // FILTRO 0: SALIDA PRIORITARIA ("Chao"/"Adiós" en vez de una pregunta real)
            if (es_frase_salida(texto)) {
                std::cout << C_GREEN "\n[OTTO] Salida detectada -> HIBERNACION\n" C_RESET << std::endl;
                otto_say(frase_aleatoria(DESPEDIDAS));
                estado = HIBERNACION;
                continue;
            }

            // FILTRO 1: Alucinaciones Whisper
            if (es_alucinacion(texto)) {
                std::cout << C_GRAY "[FILTRO] \"" << texto << "\"" << C_RESET << std::endl;
                estado = HIBERNACION;
                continue;
            }

            std::cout << C_WHITE "[STT]" C_RESET " \"" << C_BOLD << texto << C_RESET << "\"" << std::endl;
            std::string t = normalizar(texto);

            // FILTRO 2: Despedida formalizada (fallback, por si normalización lo cambia)
            if (es_despedida(t)) {
                std::cout << C_GRAY "\n[OTTO] Despedida formalizada -> HIBERNACION\n" C_RESET << std::endl;
                otto_say(frase_aleatoria(DESPEDIDAS));
                estado = HIBERNACION;
                continue;
            }

            // Validación base (lenguaje español, longitud mínima)
            if (!es_texto_valido(texto)) {
                std::cout << C_GRAY "[FILTRO] Texto invalido para LLM: \"" << texto << "\"" << C_RESET << std::endl;
                otto_say(frase_aleatoria(REPITE));
                otto_beep();
                estado = HIBERNACION;
                continue;
            }

            // El filtro semántico de vocabulario fijo (es_consulta_coherente)
            // sigue deshabilitado (ver nota del 2026-09-22 más arriba, en la
            // definición de la función) -- se confía en que Llama 3 8B
            // maneja bien preguntas raras/ambiguas por sí solo.

            std::cout << C_YELLOW "[LLM]" C_RESET " Consultando: \"" << texto << "\"" << std::endl;
            estado = PROCESANDO;
            print_indicador(PROCESANDO);

            auto t_llm_start = std::chrono::steady_clock::now();
            std::string respuesta = ollama_query(texto);
            double llm_secs = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - t_llm_start).count();
            std::cout << C_GRAY "[TIEMPO] LLM: " << llm_secs << "s" C_RESET << std::endl;

            if (respuesta.empty()) {
                otto_say(frase_aleatoria(REPITE));
                otto_beep();
            } else {
                std::cout << "\n" << C_YELLOW "[LLM]" C_RESET " Respuesta: \"" << C_BOLD << respuesta << C_RESET << "\"" << std::endl;
                auto t_tts_start = std::chrono::steady_clock::now();
                otto_say(respuesta + " " + frase_aleatoria(CONSULTA));
                double tts_secs = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - t_tts_start).count();
                std::cout << C_GRAY "[TIEMPO] TTS: " << tts_secs << "s" C_RESET << std::endl;
                otto_beep();
            }
            estado = HIBERNACION; // siempre -- una pregunta por "Hola Otto"
        }
    }

    running = false;
    cap.join();
    whisper_free(wctx);
    std::cout << "[OTTO] Pipeline detenido." << std::endl;
    return 0;
}


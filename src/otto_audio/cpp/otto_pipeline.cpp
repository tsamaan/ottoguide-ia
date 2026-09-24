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
#include <functional>
#include <cctype>
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
// initial_prompt de Whisper: condiciona el decoder, igual que si fuera el
// texto que venía antes. Son DOS prompts distintos a propósito:
//  - WAKE: el de HIBERNACION. Va cortísimo y NO contiene la frase "hola otto".
//    El initial_prompt es lo primero que Whisper alucina sobre silencio o
//    ruido, así que tener la frase completa acá haría que Otto se despierte
//    solo. Sólo sesga la ortografía del nombre.
//  - PREGUNTA: el de la pregunta real. Una frase natural con "UADE" en
//    contexto sesga muchísimo mejor que una lista de palabras sueltas, y es
//    la mitad de la solución al problema de que "UADE" volvía como "uate",
//    "u a de" o "guade" (la otra mitad es corregir_uade(), más abajo).
#define WHISPER_PROMPT_WAKE     "Otto."
#define WHISPER_PROMPT_PREGUNTA \
    "Conversación en la UADE, la Universidad Argentina de la Empresa. " \
    "Preguntas sobre carreras, campus e ingreso a UADE."
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
// Reescritos el 2026-09-23: dos decian "guia del campus"/"robot guia", del
// enfoque viejo de visitas guiadas. Ahora Otto es el robot de UADE, no un guia
// de recorridos. Cortos a proposito: los dice antes de cada pregunta.
// El saludo esta en el camino critico de la activacion: se dice ENTERO antes de
// que la persona pueda hablar. Medido el 2026-09-23: la frase mas larga que
// habia aca tardaba 3.96s en decirse, mas 1.18s que Piper tardaba en
// sintetizarla. Se acortaron todas (~3.0s) y se pre-generan al arrancar, asi el
// 1.18s de Piper sale del camino (ver pregenerar_saludos()).
const char* SALUDOS[] = {
    "Hola, soy Otto. En que te puedo ayudar?",
    "Hola! Soy Otto. Decime tu pregunta.",
    "Hola, soy Otto. Te escucho.",
    "Buenas! Soy Otto. En que te puedo ayudar?",
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

// --- Frontera de palabra ----------------------------------------------------
// Todos los filtros de este archivo buscaban sus patrones con find() a secas y
// eso muerde fuerte en español: "ya" (patrón de despedida) matchea adentro de
// "apoya", así que "¿UADE apoya a los emprendedores?" se tomaba como un chau y
// la pregunta nunca llegaba al modelo; "uate" matchea adentro de "Guatemala";
// "ende" (patrón de alucinación) matchea adentro de "entiende" y "depende".
// Acá se exige que el patrón caiga en frontera de palabra.
//
// Los bytes >= 0x80 cuentan como letra: así una vocal acentuada (2 bytes en
// UTF-8) no se parte al medio y no crea una frontera falsa.
static bool es_letra(unsigned char c) {
    return std::isalnum(c) || c >= 0x80;
}

static bool frontera(const std::string& heno, size_t pos, size_t largo) {
    bool izq = (pos == 0) || !es_letra((unsigned char)heno[pos - 1]);
    size_t fin = pos + largo;
    bool der = (fin >= heno.size()) || !es_letra((unsigned char)heno[fin]);
    return izq && der;
}

bool contiene_palabra(const std::string& heno, const std::string& aguja) {
    if (aguja.empty()) return false;
    for (size_t p = heno.find(aguja); p != std::string::npos;
         p = heno.find(aguja, p + 1))
        if (frontera(heno, p, aguja.size())) return true;
    return false;
}

int contar_palabra(const std::string& heno, const std::string& aguja) {
    if (aguja.empty()) return 0;
    int n = 0;
    for (size_t p = heno.find(aguja); p != std::string::npos;
         p = heno.find(aguja, p + 1))
        if (frontera(heno, p, aguja.size())) ++n;
    return n;
}

// Minúsculas y sin acentos, para comparar contra patrones ASCII. ::tolower es
// byte a byte y no toca UTF-8, así que las vocales acentuadas se mapean acá.
std::string plegar(const std::string& s) {
    static const struct { const char* de; char a; } ACENTOS[] = {
        {"á",'a'}, {"é",'e'}, {"í",'i'}, {"ó",'o'}, {"ú",'u'}, {"ü",'u'}, {"ñ",'n'},
        {"Á",'a'}, {"É",'e'}, {"Í",'i'}, {"Ó",'o'}, {"Ú",'u'}, {"Ü",'u'}, {"Ñ",'n'},
        {nullptr, 0}
    };
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ) {
        bool hit = false;
        for (int k = 0; ACENTOS[k].de; ++k) {
            size_t n = strlen(ACENTOS[k].de);
            if (s.compare(i, n, ACENTOS[k].de) == 0) {
                out += ACENTOS[k].a;
                i += n;
                hit = true;
                break;
            }
        }
        if (!hit) out += (char)std::tolower((unsigned char)s[i++]);
    }
    return out;
}

// --- Corregir la sigla UADE -------------------------------------------------
// "UADE" es la palabra más importante del vocabulario de Otto y la que Whisper
// escribe peor: es una sigla que en español se pronuncia como una palabra
// ("ua-de"), así que el decoder la devuelve como "uade", "Uadé", "uate",
// "U.A.D.E.", "u a de", "guade"... Si la pregunta llega al modelo sin la sigla,
// el modelo no sabe de qué universidad le están hablando y contesta cualquier
// cosa. Se atacan las dos puntas: WHISPER_PROMPT_PREGUNTA sesga el decoder y
// esta función garantiza el resultado.
//
// A diferencia de normalizar(), esta NO baja todo a minúsculas: devuelve la
// oración original con la sigla en su forma canónica, porque este es el texto
// que después se le manda al LLM.
//
// @INPUT: texto crudo de Whisper
// @OUTPUT: el mismo texto con toda variante de la sigla reemplazada por "UADE"
static const char* UADE_VARIANTES[] = {
    "uade", "uad", "uate", "uage", "uadi", "uader",
    "puade", "wuade", "guade", "buade", "huade", "juade",
    "ude",
    nullptr
};

// Formas deletreadas: la sigla parte en varios tokens ("u a de", "U.A.D.E.").
// Se aceptan por la concatenación de las letras, no por una lista de espacios.
static const char* UADE_CONCAT[] = { "uade", "uhade", nullptr };

namespace {
struct Token { std::string texto; bool palabra; };

std::vector<Token> tokenizar(const std::string& s) {
    std::vector<Token> tks;
    size_t i = 0;
    while (i < s.size()) {
        bool pal = es_letra((unsigned char)s[i]);
        size_t j = i;
        while (j < s.size() && es_letra((unsigned char)s[j]) == pal) ++j;
        tks.push_back({s.substr(i, j - i), pal});
        i = j;
    }
    return tks;
}

bool en_lista(const char* const* lista, const std::string& v) {
    for (int k = 0; lista[k]; ++k)
        if (v == lista[k]) return true;
    return false;
}
} // namespace

std::string corregir_uade(const std::string& texto) {
    std::vector<Token> tks = tokenizar(texto);
    std::string out;
    for (size_t i = 0; i < tks.size(); ++i) {
        if (!tks[i].palabra) { out += tks[i].texto; continue; }

        // Se prueban primero los runs largos: "u a d e" antes que "u" sola.
        size_t consume = 0;
        for (size_t largo = 4; largo >= 1; --largo) {
            std::string concat;
            size_t ultimo = i, vistas = 0;
            bool ok = true;
            for (size_t j = i; j < tks.size() && vistas < largo; ++j) {
                if (tks[j].palabra) {
                    concat += plegar(tks[j].texto);
                    ultimo = j;
                    ++vistas;
                } else if (vistas > 0) {
                    // Entre letra y letra de la sigla sólo se tolera un
                    // separador corto (" ", ". ", "-"). Un salto de línea o
                    // algo más largo ya es otra frase.
                    const std::string& sep = tks[j].texto;
                    if (sep.size() > 2 || sep.find('\n') != std::string::npos) {
                        ok = false;
                        break;
                    }
                }
            }
            if (!ok || vistas != largo) continue;
            bool match = (largo == 1) ? en_lista(UADE_VARIANTES, concat)
                                      : en_lista(UADE_CONCAT, concat);
            if (match) { consume = ultimo - i + 1; break; }
        }

        if (consume) {
            out += "UADE";
            i += consume - 1;
        } else {
            out += tks[i].texto;
        }
    }
    return out;
}

// --- Normalizar texto -------------------------------------------------------
// Forma canónica para que los filtros (wake word, despedida) comparen contra
// una sola versión del texto: sigla corregida, minúsculas, sin acentos.
// Antes esto hacía reemplazos de substring a mano ({"uate","uade"}, ...), lo
// que además de duplicar la lista de variantes corrompía palabras que la
// contenían ("Guatemala" -> "Guademala"). Ahora la sigla la maneja
// corregir_uade(), que trabaja por token.
std::string normalizar(const std::string& raw) {
    std::string s = plegar(corregir_uade(raw));

    // Variantes del nombre del robot: es lo único que le importa al wake word.
    static const struct { const char* de; const char* a; } NOMBRE[] = {
        {"otto mann", "otto"}, {"otto man", "otto"},
        {"ottoman", "otto"}, {"otoman", "otto"},
        {nullptr, nullptr}
    };
    for (int i = 0; NOMBRE[i].de; ++i) {
        size_t pos;
        while ((pos = s.find(NOMBRE[i].de)) != std::string::npos)
            s.replace(pos, strlen(NOMBRE[i].de), NOMBRE[i].a);
    }
    return s;
}

// --- Filtro anti-alucinaciones ----------------------------------------------
// Dos listas en vez de una: antes había una sola y el código decidía sobre la
// marcha, con casos especiales por nombre (`p == "ottoguide"`), si el patrón
// alcanzaba con aparecer una vez o tenía que repetirse. Eso dejaba dos huecos:
//   - "suscribite", "sous-titrage" y "radio-canada" exigían DOS apariciones, y
//     con una sola ya es una alucinación cantada.
//   - el patrón de subtitulado era "subtitl", que es el prefijo del inglés
//     "subtitles"; la alucinación que realmente tira Whisper en español es
//     "Subtítulos realizados por la comunidad de Amara.org" -> "subtitul".
//     Nunca se filtró. (app.py sí la filtraba, por eso sólo se veía acá.)
bool es_alucinacion(const std::string& t) {
    if (t.empty() || t.size() < 8) return true;

    std::string tl = plegar(t);

    // Con una sola aparición ya alcanza: artefactos del decoder, eco del
    // initial_prompt y las frases de subtitulado que Whisper inventa cuando le
    // entra ruido o música de fondo.
    static const char* UNA_VEZ[] = {
        "*", "[", "\xe2\x99\xaa",
        "subtitl", "subtitul", "amara.org", "sous-titrage", "radio-canada",
        "suscribite", "suscribete", "suscribanse",
        "otto otto", "otto guide", "ottoguide", "uade otto",
        "eh eh",
        nullptr
    };
    for (int i = 0; UNA_VEZ[i]; ++i)
        if (tl.find(UNA_VEZ[i]) != std::string::npos) return true;

    // Sólo sospechosas si se repiten: también aparecen en preguntas legítimas.
    // Se cuentan como palabra entera (contar_palabra): con find() a secas
    // "ende" matcheaba adentro de "entiende" y "depende", así que una pregunta
    // como "¿de qué depende la beca? no entiendo" se descartaba como
    // alucinación y nunca llegaba al modelo.
    static const char* REPETIDAS[] = {
        "empresa", "gracias", "eheh", "ehe", "mmm", "hmm", "ugh",
        "ende", "udrundr",
        nullptr
    };
    for (int i = 0; REPETIDAS[i]; ++i)
        if (contar_palabra(tl, REPETIDAS[i]) >= 2) return true;

    return false;
}

// --- Wake word y despedida --------------------------------------------------
bool es_wake_word(const std::string& t) {
    for (auto& w : {"hola otto","ola otto","hola oto","ola oto","hola auto"})
        if (t.find(w) != std::string::npos) return true;
    return false;
}

// Despedida explicita. Recibe texto ya pasado por normalizar().
//
// Antes habia DOS funciones (es_frase_salida sobre el texto crudo y es_despedida
// sobre el normalizado) con listas parecidas de palabras sueltas: "chau",
// "adios", "ya", "listo". Eso se comia preguntas legitimas -- "¿UADE apoya a
// los emprendedores?" salia por "ya" dentro de "apoya" -- asi que quedaron
// unificadas en una sola que EXIGE el nombre del robot. Es el unico filtro que
// corre antes del LLM sobre la pregunta, y con el nombre pedido no hay
// ambiguedad posible.
bool es_despedida_de_otto(const std::string& t) {
    if (!contiene_palabra(t, "otto")) return false;
    // Solo palabras de despedida inequivocas. NO van "ya", "listo" ni "gracias"
    // aunque este el nombre: la gente le habla al robot por su nombre, y
    // "Otto, ¿ya estan las inscripciones?" o "Otto, gracias, ¿que carreras
    // tiene UADE?" son preguntas, no despedidas.
    for (auto& w : {"chau","chao","adios","bye",
                    "hasta luego","hasta pronto","nos vemos","hasta la vista"})
        if (contiene_palabra(t, w)) return true;
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
// Decodifica un valor JSON escapado ("\\n", "\\"", "\\\\") a texto plano.
//
// "\\n" se devuelve como salto de linea DE VERDAD, no como espacio. Esto parece
// un detalle y era un bug de fondo: limpiar_para_voz() saca las viñetas y la
// numeracion solo al principio de linea, pero este decodificador aplanaba los
// saltos antes, asi que no habia principio de linea que detectar y esa parte
// del sanitizador nunca se ejecutaba. Resultado: el modelo mandaba
// "1. Revisa la web  2. Escribi a Bedelia" y Piper leia "uno punto",
// "dos punto" en voz alta. Los saltos igual terminan como espacio: los aplana
// limpiar_para_voz() al final, que es donde corresponde.
static std::string desescapar_json(const std::string& src) {
    std::string out;
    out.reserve(src.size());
    for (size_t i = 0; i < src.size(); ++i) {
        if (src[i] != '\\' || i + 1 >= src.size()) { out += src[i]; continue; }
        switch (src[++i]) {
            case 'n':                     out += '\n'; break;
            case 'r':                     out += '\r'; break;
            case 't':                     out += ' ';  break;
            case '"':                     out += '"';  break;
            case '\\':                    out += '\\'; break;
            default:                      out += src[i];
        }
    }
    return out;
}

// Extrae el valor de "response" de una linea NDJSON de Ollama, o "" si no hay.
static std::string campo_response(const std::string& linea) {
    static const std::string KEY = "\"response\":\"";
    size_t a = linea.find(KEY);
    if (a == std::string::npos) return "";
    a += KEY.size();
    // Buscar la comilla de cierre sin cortar en una comilla escapada.
    for (size_t i = a; i < linea.size(); ++i) {
        if (linea[i] == '\\') { ++i; continue; }
        if (linea[i] == '"') return desescapar_json(linea.substr(a, i - a));
    }
    return "";
}

// --- Consulta a Ollama, en streaming ----------------------------------------
// Antes se pedia con "stream":false y se esperaba la respuesta COMPLETA antes
// de empezar a hablar. Medido el 2026-09-23: 100 tokens a 9.3 tok/s = 10.9s de
// silencio absoluto antes de que Otto abriera la boca (y el prompt del Modelfile
// no tiene nada que ver: se evalua en 0.2s porque Ollama lo tiene cacheado).
// Ahora se lee token por token y se llama a `on_oracion` en cuanto hay una
// oracion completa, asi Otto empieza a hablar a los ~2.5s. El total no baja,
// pero la espera percibida se derrumba.
//
// Mientras `on_oracion` habla (segundos) no se lee el socket. No es problema:
// la respuesta entera son ~500 bytes, entra de sobra en el buffer del socket, y
// de hecho es justo lo que se quiere -- el modelo sigue generando la oracion
// siguiente mientras Otto dice la actual.
//
// `max_oraciones` corta de raiz el problema de que el modelo no se calla. El
// Modelfile le pide DOS oraciones y no obedece: llega siempre al tope de
// num_predict y hay que truncarlo a mitad de palabra. Rogarle al prompt ya se
// intento (ver Modelfile, 2026-09-23). Con streaming el consumidor decide, y es
// una garantia y no un pedido: al llegar al limite se cierra el socket, Ollama
// ve el cliente desconectado y deja de generar. Ademas ahorra tiempo, porque ya
// no se esperan los tokens 50 a 100 que igual no se iban a decir.
//
// @INPUT: pregunta; on_oracion = se invoca con cada oracion completa;
//         max_oraciones = cuantas se dicen antes de cortar
// @OUTPUT: la respuesta completa dicha (para loguear), o "" si fallo la conexion
std::string ollama_query_stream(
        const std::string& pregunta,
        const std::function<void(const std::string&)>& on_oracion,
        int max_oraciones = 2) {
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

    // keep_alive:-1 => el modelo queda residente en la GPU para siempre. Sin
    // esto Ollama lo descarga a los 5 minutos de inactividad, y la siguiente
    // pregunta paga ~47s de recarga (4.7GB a la GPU) en vez de ~3s -- que es
    // justo el caso de uso real (alguien pregunta, pasa un rato, otro
    // pregunta). Medido el 2026-09-23: 51s en frío vs 3.7s en caliente.
    std::string body = "{\"model\":\"otto-llama3\",\"prompt\":\"" + p
                     + "\",\"stream\":true,\"think\":false,\"keep_alive\":-1}";
    std::string req  = "POST /api/generate HTTP/1.0\r\n"
                       "Host: 127.0.0.1\r\n"
                       "Content-Type: application/json\r\n"
                       "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    send(sock, req.c_str(), req.size(), 0);

    std::string completa, oracion, pendiente;
    char buf[4096];
    int n;

    // Se corta la oracion en . ! ? ... pero recien pasados MIN_ORACION
    // caracteres, para no mandarle a Piper fragmentos de dos palabras (que
    // suenan cortados) ni partir un numero decimal. MAX_ORACION es la valvula
    // de escape para una respuesta sin puntuacion.
    const size_t MIN_ORACION = 40, MAX_ORACION = 220;

    // `completa` se arma DENTRO de soltar(), no al recibir cada token: asi lo que
    // se devuelve y se loguea es exactamente lo que Otto dijo, y no incluye la
    // oracion a medias que quedo en el buffer cuando se corto la generacion.
    int dichas = 0;
    auto soltar = [&]() {
        if (oracion.empty()) return;
        on_oracion(oracion);
        completa += oracion;
        oracion.clear();
        ++dichas;
    };

    while ((n = recv(sock, buf, sizeof(buf) - 1, 0)) > 0) {
        buf[n] = 0;
        pendiente += buf;
        // Ollama responde NDJSON: un objeto por linea. Si el servidor usa
        // Transfer-Encoding: chunked, las lineas de tamanio hexadecimal no
        // contienen "response" y campo_response() las ignora sola, asi que no
        // hace falta un parser de chunked.
        size_t nl;
        while ((nl = pendiente.find('\n')) != std::string::npos) {
            std::string linea = pendiente.substr(0, nl);
            pendiente.erase(0, nl + 1);
            std::string trozo = campo_response(linea);
            if (trozo.empty()) continue;
            oracion += trozo;
            char ult = oracion.empty() ? 0 : oracion[oracion.size() - 1];
            bool fin_oracion = (ult == '.' || ult == '!' || ult == '?');
            if ((fin_oracion && oracion.size() >= MIN_ORACION) || oracion.size() >= MAX_ORACION)
                soltar();
            if (dichas >= max_oraciones) break;
        }
        if (dichas >= max_oraciones) break;
    }
    // Cerrar el socket con el modelo a medio generar es a proposito: es la
    // senial para que Ollama abandone la generacion.
    close(sock);
    if (dichas < max_oraciones) soltar();   // lo que quedo sin punto final
    return completa;
}

// --- Limpiar la respuesta del LLM antes de mandarla a voz --------------------
// Llama 3 8B NO obedece de forma confiable la regla de "sin listas ni markdown"
// del SYSTEM: ante preguntas de enumeracion insiste en responder con vinetas y
// asteriscos (medido el 2026-09-23, incluso despues de agregar ejemplos
// few-shot y la prohibicion explicita). Y eso Piper lo lee literal: "asterisco
// Ingenieria Industrial asterisco...". Asi que no se le pide al modelo, se
// limpia aca: es deterministico y no depende de que el modelo colabore.
std::string limpiar_para_voz(const std::string& texto) {
    // Primero los links markdown: "[texto](url)" -> "texto". El modelo los mete
    // al dar mails o webs (visto el 2026-09-23: "[ingreso@uade.edu.ar]
    // (mailto:ingreso@uade.edu.ar)"), y Piper leeria los corchetes y el
    // "mailto" en voz alta.
    std::string sin_links;
    sin_links.reserve(texto.size());
    for (size_t i = 0; i < texto.size(); ++i) {
        if (texto[i] == '[') {
            size_t cierre = texto.find(']', i);
            if (cierre != std::string::npos) {
                sin_links += texto.substr(i + 1, cierre - i - 1);
                i = cierre;
                // Si sigue un "(...)", es la URL del link: se descarta entera.
                if (i + 1 < texto.size() && texto[i + 1] == '(') {
                    size_t fin = texto.find(')', i + 1);
                    if (fin != std::string::npos) i = fin;
                }
                continue;
            }
        }
        sin_links += texto[i];
    }

    std::string out;
    out.reserve(sin_links.size());

    bool inicio_de_linea = true;
    for (size_t i = 0; i < sin_links.size(); ++i) {
        char c = sin_links[i];

        // Saltos de linea -> espacio: es una sola tirada de voz, no un texto.
        if (c == '\n' || c == '\r') {
            inicio_de_linea = true;
            if (!out.empty() && out.back() != ' ') out += ' ';
            continue;
        }

        // Vinetas al principio de linea ("* ", "- ", "1. ") -> se descartan.
        if (inicio_de_linea) {
            if (c == ' ' || c == '\t') continue;
            if (c == '*' || c == '-' || c == '+') {
                if (i + 1 < sin_links.size()
                    && (sin_links[i+1] == ' ' || sin_links[i+1] == '\t')) continue;
            }
            if (isdigit((unsigned char)c) && i + 1 < sin_links.size()
                && (sin_links[i+1] == '.' || sin_links[i+1] == ')')) { ++i; continue; }
            inicio_de_linea = false;
        }

        // Marcas de markdown que no significan nada hablado.
        if (c == '*' || c == '_' || c == '`' || c == '#') continue;

        out += c;
    }

    // Espacios repetidos -> uno solo.
    std::string limpio;
    limpio.reserve(out.size());
    for (char c : out) {
        if (c == ' ' && !limpio.empty() && limpio.back() == ' ') continue;
        limpio += c;
    }
    while (!limpio.empty() && (limpio.front() == ' ')) limpio.erase(limpio.begin());
    while (!limpio.empty() && (limpio.back() == ' ')) limpio.pop_back();
    return limpio;
}

// --- TTS + reproduccion -----------------------------------------------------
// Separado en generar / reproducir para poder pre-generar audios fijos (los
// saludos) y sacar el tiempo de Piper del camino critico. Ver
// pregenerar_saludos().
//
// @INPUT: texto a sintetizar; salida = ruta del WAV final (16kHz mono s16)
// @OUTPUT: false si Piper o ffmpeg no dejaron un WAV usable
bool tts_generar(const std::string& texto, const std::string& salida) {
    std::ofstream f("/tmp/otto_pipe_text.txt");
    f << texto;
    f.close();

    system("cat /tmp/otto_pipe_text.txt | " PIPER_BIN
           " --model " PIPER_VOICE
           " --output_file /tmp/otto_pipe_raw.wav >/dev/null 2>&1");

    std::string cmd = "ffmpeg -y -i /tmp/otto_pipe_raw.wav -ar 16000 -ac 1 "
                      "-sample_fmt s16 -af \"volume=3.0\" " + salida
                    + " -loglevel quiet";
    system(cmd.c_str());

    int32_t sr = -1; int8_t ch = 0; bool ok = false;
    ReadWave(salida, &sr, &ch, &ok);
    if (!ok || sr != 16000 || ch != 1) {
        std::cerr << "[TTS] Error generando WAV para: " << texto.substr(0, 40)
                  << std::endl;
        return false;
    }
    return true;
}

void reproducir_wav(const std::string& ruta) {
    int32_t sr = -1; int8_t ch = 0; bool ok = false;
    auto pcm = ReadWave(ruta, &sr, &ch, &ok);
    if (!ok || sr != 16000 || ch != 1) {
        std::cerr << "[TTS] Error WAV: " << ruta << std::endl; return;
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

void otto_say(const std::string& texto) {
    if (tts_generar(texto, "/tmp/otto_pipe.wav"))
        reproducir_wav("/tmp/otto_pipe.wav");
}

// Saludos pre-generados. Piper tarda ~1.18s en sintetizar un saludo (medido en
// el robot el 2026-09-23) y son 4 frases fijas, asi que se generan una sola vez
// al arrancar -- durante la carga de Whisper, que ya se lleva ~15s y no le
// molesta compartir. Al detectar el wake word solo queda reproducir un WAV.
std::vector<std::string> saludos_wav;

void pregenerar_saludos() {
    for (int i = 0; SALUDOS[i]; ++i) {
        std::string ruta = "/tmp/otto_saludo_" + std::to_string(i) + ".wav";
        if (tts_generar(SALUDOS[i], ruta)) saludos_wav.push_back(ruta);
    }
    std::cout << C_GRAY "[TTS] Saludos pre-generados: " << saludos_wav.size()
              << "/" << ((int)(sizeof(SALUDOS)/sizeof(SALUDOS[0])) - 1)
              << C_RESET << std::endl;
}

// Si la pre-generacion fallo se cae al camino normal (sintetizar en el momento)
// en vez de quedarse mudo: degradacion gradual, igual que el VAD con RmsVad.
void otto_saludar() {
    if (saludos_wav.empty()) { otto_say(frase_aleatoria(SALUDOS)); return; }
    reproducir_wav(saludos_wav[rand() % saludos_wav.size()]);
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
// rapido: usa muestreo greedy (para el chequeo de wake word). Ver el comentario
// en la eleccion de sampling, mas abajo.
std::string transcribir(whisper_context* ctx, const std::vector<int16_t>& pcm_i16,
                         bool rapido = false) {
    // Normalizar amplitud para mejorar precision de Whisper
    float max_val = 1.0f;
    for (auto s : pcm_i16)
        max_val = std::max(max_val, std::abs((float)s));
    float gain = (max_val > 500.0f) ? (20000.0f / max_val) : 1.0f;

    std::vector<float> pcm_f32(pcm_i16.size());
    for (size_t i = 0; i < pcm_i16.size(); ++i)
        pcm_f32[i] = std::min(1.0f, std::max(-1.0f, (pcm_i16[i] * gain) / 32768.0f));

    // rapido=true (HIBERNACION): greedy en vez de beam search. Para decidir si
    // se dijo "hola otto" no hace falta la calidad del beam, y es bastante mas
    // rapido -- en HIBERNACION se transcribe todo lo que se escucha, asi que
    // cada milisegundo se paga muchas veces. Para la pregunta real
    // (ESCUCHANDO) se sigue usando beam search, ahi la calidad si importa.
    whisper_full_params params = whisper_full_default_params(
        rapido ? WHISPER_SAMPLING_GREEDY : WHISPER_SAMPLING_BEAM_SEARCH);
    params.language         = "es";
    params.print_progress   = false;
    params.print_realtime   = false;
    params.print_timestamps = false;
    params.no_context       = true;
    // Prompt distinto segun la pasada: ver WHISPER_PROMPT_WAKE / _PREGUNTA.
    params.initial_prompt   = rapido ? WHISPER_PROMPT_WAKE : WHISPER_PROMPT_PREGUNTA;
    params.n_threads        = 4;
    if (!rapido) params.beam_search.beam_size = 3;
    params.no_speech_thold  = 0.4f;
    params.temperature      = 0.0f;

    // audio_ctx: por defecto Whisper rellena el audio hasta la ventana entera
    // de 30s y el encoder paga ese costo completo, aunque el chunk tenga 2
    // segundos. En la pasada del wake word el chunk nunca pasa de 2.2s, asi que
    // recortar el contexto del encoder a 256 frames (= 5.12s, margen de sobra)
    // baja la pasada de ~1.0s a ~0.3s. Medido en el robot el 2026-09-23 con
    // whisper-cli sobre un WAV de 2.86s: 1500 -> 3.87s, 512 -> 3.32s,
    // 256 -> 3.18s de wall time (de los cuales ~2.9s son cargar el modelo, asi
    // que la inferencia real cae ~3x). Con 128 se rompe: el decoder entra en
    // loop, repite la frase y tarda 19s.
    // En la pregunta real se deja el default: ahi la calidad importa y el chunk
    // puede llegar a 8s.
    if (rapido) params.audio_ctx = 256;

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

    // Sintetizar los saludos ahora, no cuando alguien diga "Hola Otto": saca
    // los ~1.18s de Piper del camino critico de la activacion.
    pregenerar_saludos();

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
          
    // El indicador se imprime solo al CAMBIAR de estado. Antes se reimprimia en
    // cada vuelta del loop con "\r": en una terminal se ve bien, pero al correr
    // con nohup redirigido a un archivo (que es como lo lanza la web) el log
    // quedaba con cientos de "[◯] HIBERNACION esperando..." pegados en una
    // linea, ilegible.
    State estado_impreso = PROCESANDO; // distinto del inicial: fuerza el primer print

    while (running) {
        if (estado != estado_impreso) {
            print_indicador(estado);
            std::cout << std::endl;
            estado_impreso = estado;
        }

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
            // ms_max corto (2500): "Hola Otto" dura menos de 1.5s. Con el
            // default de 8s, cualquier charla ajena cerca del robot se
            // capturaba entera y se mandaba a Whisper -- medido en el log del
            // 2026-09-23: 4.2s de GPU para transcribir "Fijate con eso, capaz
            // que estoy diciendo una boludez...". Y mientras Whisper laburaba,
            // el pipeline NO estaba escuchando: un "Hola Otto" dicho justo en
            // ese rato se perdia.
            auto chunk = tomar_utterance(*vad, 300, 500, 2500);
            if (chunk.empty()) continue;

            // Si la captura llego casi al tope, la persona seguia hablando:
            // nadie dice "Hola Otto" y sigue de largo sin pausa. No vale
            // gastar Whisper en eso.
            if (chunk.size() > (size_t)(SAMPLE_RATE * 2200 / 1000)) {
                std::cout << C_GRAY "[◯] charla larga, no es wake word" C_RESET << std::endl;
                continue;
            }

            // rapido=true -> greedy: en HIBERNACION se transcribe todo lo que
            // se escucha, asi que la velocidad importa mas que la calidad.
            std::string texto = transcribir(wctx, chunk, /*rapido=*/true);
            if (texto.empty()) continue;

            std::string t = normalizar(texto);

            // El wake word se chequea ANTES del filtro de alucinaciones (bug
            // encontrado el 2026-09-23 leyendo el log): "otto otto" esta en la
            // lista de patrones de alucinacion, asi que un "Hola Otto" que
            // Whisper transcribia como "Otto Otto" se descartaba y NUNCA
            // activaba. En el log aparecia como: [FILTRO] "Otto Otto".
            if (es_wake_word(t)) {
                float rms = calcular_rms(chunk);
                std::cout << "\n" << C_CYAN "[MIC]" C_RESET " " << rms_bar(rms, RMS_THRESHOLD)
                          << " RMS:" << C_BOLD << (int)rms << C_RESET << std::endl;
                std::cout << C_WHITE "[STT]" C_RESET " \"" << C_BOLD << texto << C_RESET << "\"" << std::endl;
                std::cout << C_GREEN C_BOLD "[OTTO] Wake word detectada -> ESCUCHANDO" C_RESET << std::endl;
                estado = ESCUCHANDO;
                otto_saludar();
                otto_beep();
                continue;
            }

            // No era el wake word: una sola linea corta y seguimos. Antes se
            // imprimia la barra de RMS + "Transcribiendo..." + el texto entero
            // de cada charla ajena, y el log quedaba ilegible.
            std::cout << C_GRAY "[◯] descartado: \"" << texto.substr(0, 45)
                      << (texto.size() > 45 ? "...\"" : "\"") << C_RESET << std::endl;
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
            // El indicador ya lo imprimio el chequeo de cambio de estado arriba.
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

            // Se imprime la transcripcion ANTES de cualquier filtro. Antes se
            // imprimia despues, y cuando un filtro se comia la pregunta el log
            // decia que la habia descartado pero no QUE habia escuchado -- justo
            // el caso que hay que diagnosticar. (Prueba del 2026-09-23: dos de
            // tres preguntas murieron en el filtro de salida y no hubo forma de
            // saber cual era el texto.)
            std::cout << C_WHITE "[STT]" C_RESET " \"" << C_BOLD << texto << C_RESET << "\"" << std::endl;

            // FILTRO 0: despedida. Ahora EXIGE el nombre del robot ("chau otto"),
            // no alcanza un "chau" suelto. Con el diseño actual -- una pregunta
            // por "Hola Otto", sin sesion extendida -- no hay ninguna sesion de
            // la cual salir: la persona dice el wake word justamente porque
            // quiere preguntar algo, asi que este filtro solo podia restar. Y
            // restaba: en la prueba del 2026-09-23 se comio dos de tres
            // preguntas. Con el nombre pedido el falso positivo es practicamente
            // imposible ("chau otto" no aparece dentro de una pregunta) y Otto
            // se sigue despidiendo lindo si alguien efectivamente se despide.
            if (es_despedida_de_otto(normalizar(texto))) {
                std::cout << C_GREEN "[OTTO] Despedida -> HIBERNACION" C_RESET << std::endl;
                otto_say(frase_aleatoria(DESPEDIDAS));
                estado = HIBERNACION;
                continue;
            }

            // FILTRO 1: Alucinaciones Whisper
            if (es_alucinacion(texto)) {
                std::cout << C_GRAY "[FILTRO] alucinacion -> HIBERNACION" C_RESET << std::endl;
                estado = HIBERNACION;
                continue;
            }

            // Validación base (lenguaje español, longitud mínima)
            if (!es_texto_valido(texto)) {
                std::cout << C_GRAY "[FILTRO] texto invalido para el LLM -> pido que repita" C_RESET << std::endl;
                otto_say(frase_aleatoria(REPITE));
                otto_beep();
                estado = HIBERNACION;
                continue;
            }

            // El filtro semántico de vocabulario fijo (es_consulta_coherente)
            // sigue deshabilitado (ver nota del 2026-09-22 más arriba, en la
            // definición de la función) -- se confía en que Llama 3 8B
            // maneja bien preguntas raras/ambiguas por sí solo.

            // Lo que se le manda al LLM lleva la sigla corregida (ver
            // corregir_uade): hasta ahora se mandaba el texto crudo de Whisper,
            // asi que si habia escrito "uate" o "u a de" el modelo no sabia de
            // que universidad le hablaban. Se imprime la pregunta corregida, no
            // la cruda, para que el log muestre lo que el modelo realmente vio.
            std::string pregunta = corregir_uade(texto);
            std::cout << C_YELLOW "[LLM]" C_RESET " Consultando: \"" << pregunta << "\"" << std::endl;
            estado = PROCESANDO;
            print_indicador(PROCESANDO);

            // Streaming: Otto habla cada oracion en cuanto el modelo la
            // termina, en vez de esperar la respuesta entera. El total no baja
            // (el cuello es 9.3 tok/s de generacion) pero la espera hasta la
            // primera palabra pasa de ~11s a ~2.5s.
            auto t_llm_start = std::chrono::steady_clock::now();
            auto desde_inicio = [&]() {
                return std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - t_llm_start).count();
            };

            bool hablo = false;
            std::string respuesta = ollama_query_stream(pregunta,
                [&](const std::string& frase) {
                    std::string limpia = limpiar_para_voz(frase);
                    if (limpia.empty()) return;
                    if (!hablo) {
                        std::cout << C_GRAY "[TIEMPO] primera oracion a los "
                                  << desde_inicio() << "s" C_RESET << std::endl;
                        hablo = true;
                    }
                    std::cout << C_YELLOW "[TTS]" C_RESET " \"" << limpia << "\"" << std::endl;
                    otto_say(limpia);
                });

            if (!hablo) {
                // Ni una oracion: se cayo la conexion con Ollama o vino vacio.
                std::cout << C_GRAY "[LLM] sin respuesta -> pido que repita" C_RESET << std::endl;
                otto_say(frase_aleatoria(REPITE));
                otto_beep();
            } else {
                otto_say(frase_aleatoria(CONSULTA));
                std::cout << "\n" << C_YELLOW "[LLM]" C_RESET " Respuesta completa: \""
                          << C_BOLD << respuesta << C_RESET << "\"" << std::endl;
                std::cout << C_GRAY "[TIEMPO] total LLM+TTS: " << desde_inicio()
                          << "s" C_RESET << std::endl;
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


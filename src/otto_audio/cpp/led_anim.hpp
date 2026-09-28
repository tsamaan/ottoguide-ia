// Animacion del LED del G1 mientras suena el parlante.
//
// POR QUE EXISTE
// Cuando Otto habla, la unica senial es el audio. Si hay ruido de sala, o la
// persona esta de costado, no hay forma de saber si el robot esta contestando o
// si se colgo. El LED acompaniando el audio resuelve eso de un vistazo.
//
// DONDE SE ENGANCHA
// En los DOS binarios que reproducen audio, que entre los dos cubren los tres
// caminos:
//   - otto_pipeline   -> reproducir_wav()  = el ciclo de "Hola Otto"
//   - otto_speak_file -> main()            = la botonera Y las respuestas de GPT
//     (otto_preset.sh y ask_gpt_and_speak.py lo invocan a el)
//
// COMO
// Un hilo aparte pulsa el LED mientras dura la reproduccion. Va en un hilo y no
// intercalado con el envio de audio porque reproducir_wav() se pasa la mayor
// parte del tiempo dormido esperando que termine el audio: desde ahi no hay
// donde meter la animacion sin cortar el sonido.
#ifndef OTTO_LED_ANIM_HPP
#define OTTO_LED_ANIM_HPP

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include <unitree/robot/g1/audio/g1_audio_client.hpp>

// Color base del PULSO (mientras habla). Es el verde-azulado que usa la web
// (--accent), para que el robot y la interfaz se lean como el mismo sistema, y
// se eligio distinto del azul de reposo a proposito: si fueran el mismo color,
// hablar se veria apenas como un cambio de brillo y el punto de todo esto es
// que se note de un vistazo.
#ifndef LED_R
#define LED_R 0
#define LED_G 180
#define LED_B 200
#endif

// Color de REPOSO: a donde vuelve el LED cuando Otto termina de hablar.
//
// No es (0,0,0) y esto importa: el G1 tiene una luz fija encendida cuando esta
// tranquilo, y apagarla al terminar de hablar deja al robot "muerto" a la vista.
//
// LO IDEAL SERIA leer el color justo antes de empezar a hablar y restaurar ese.
// NO SE PUEDE: el API de audio del SDK tiene 7 operaciones y para el LED existe
// solo SET_RGB_LED (1010). Hay GET_VOLUME (1005) emparejado con SET_VOLUME
// (1006), pero no hay ningun GET_RGB_LED en todo el SDK. El robot no puede
// informar de que color esta su propio LED.
//
// Entonces se lo recuerda: ARCHIVO_ESTADO guarda el ultimo color que alguien
// fijo a proposito, y ahi vuelve la animacion. Va en /tmp y eso es deliberado,
// no pereza: el archivo tiene que morir con el arranque, porque despues de un
// reinicio el firmware del robot pone su propio color y nuestra memoria seria
// mentira. Sin archivo (primer audio despues de bootear) se usa el default.
//
// Queda un hueco que no se puede cerrar: si el propio robot cambia el LED por su
// cuenta (bateria, error), no nos enteramos y restauramos un color viejo.
#ifndef LED_ARCHIVO_ESTADO
#define LED_ARCHIVO_ESTADO "/tmp/otto_led_actual"
#endif

// Orden de precedencia para saber a donde volver: lo que alguien fijo (archivo),
// despues la variable de entorno (para probar sin recompilar), despues el
// default compilado.
#ifndef LED_REPOSO_R
#define LED_REPOSO_R 0
#define LED_REPOSO_G 0
#define LED_REPOSO_B 255
#endif

// Periodo de una respiracion completa (oscuro -> brillante -> oscuro).
#ifndef LED_CICLO_MS
#define LED_CICLO_MS 900
#endif

// Cada cuanto se manda un color nuevo. LedControl es una llamada RPC al robot,
// no un registro de hardware: mandarla a 60fps seria inundar el bus interno
// para una diferencia que nadie ve. ~16 por segundo ya se ve fluido.
#ifndef LED_PASO_MS
#define LED_PASO_MS 60
#endif

// Deja el LED en su color de reposo. Suelta (y no solo un metodo de la clase)
// porque tambien la necesita el camino de salida por senial, donde puede no
// haber ningun LedHablando vivo a mano.
// Parsea "R,G,B" con rango valido. Si viene mal escrito NO se toca nada: un
// color raro por un typo es peor que el default, y no hay a quien reportarle el
// error desde aca.
inline bool led_parsear(const char* texto, uint8_t& r, uint8_t& g, uint8_t& b) {
    if (!texto) return false;
    int rr = 0, gg = 0, bb = 0;
    if (std::sscanf(texto, "%d,%d,%d", &rr, &gg, &bb) != 3) return false;
    if (rr < 0 || rr > 255 || gg < 0 || gg > 255 || bb < 0 || bb > 255) return false;
    r = (uint8_t)rr; g = (uint8_t)gg; b = (uint8_t)bb;
    return true;
}

inline void led_color_reposo(uint8_t& r, uint8_t& g, uint8_t& b) {
    r = LED_REPOSO_R; g = LED_REPOSO_G; b = LED_REPOSO_B;
    if (led_parsear(std::getenv("OTTO_LED_REPOSO"), r, g, b)) return;
    std::FILE* f = std::fopen(LED_ARCHIVO_ESTADO, "r");
    if (!f) return;
    char linea[64] = {0};
    if (std::fgets(linea, sizeof(linea), f)) led_parsear(linea, r, g, b);
    std::fclose(f);
}

inline void led_a_reposo(unitree::robot::g1::AudioClient* audio) {
    if (!audio) return;
    uint8_t r, g, b;
    led_color_reposo(r, g, b);
    audio->LedControl(r, g, b);
}

// Fija un color Y lo recuerda, para que la animacion sepa a donde volver.
// Esta es la funcion que tiene que usar cualquier cosa que quiera cambiar el
// color de Otto a proposito; LedControl a secas se lo olvidaria.
inline void led_fijar_reposo(unitree::robot::g1::AudioClient* audio,
                             uint8_t r, uint8_t g, uint8_t b) {
    if (audio) audio->LedControl(r, g, b);
    std::FILE* f = std::fopen(LED_ARCHIVO_ESTADO, "w");
    if (!f) return;
    std::fprintf(f, "%d,%d,%d\n", (int)r, (int)g, (int)b);
    std::fclose(f);
}

// Pulsa el LED mientras el objeto vive.
//
// Es RAII a proposito: reproducir_wav() tiene un `return` temprano si el WAV no
// se puede leer, y con un parar() manual ese camino dejaria el LED prendido para
// siempre, sin nada que lo apague hasta el proximo audio. Con el destructor, se
// apaga incluso si la funcion sale por el medio.
class LedHablando {
public:
    explicit LedHablando(unitree::robot::g1::AudioClient* audio)
        : audio_(audio), corriendo_(audio != nullptr) {
        if (!corriendo_) return;
        hilo_ = std::thread([this] { bucle(); });
    }

    ~LedHablando() { parar(); }

    // Copiar o mover esto significaria dos hilos sobre el mismo LED, o un
    // destructor apagando lo que el otro prendio.
    LedHablando(const LedHablando&) = delete;
    LedHablando& operator=(const LedHablando&) = delete;

    void parar() {
        if (!corriendo_.exchange(false)) return;
        if (hilo_.joinable()) hilo_.join();
        // Volver al reposo DESPUES del join: si se hiciera antes, el hilo
        // podria meter un color mas entre medio y el LED quedaria en el
        // ultimo paso del pulso en vez de en su estado normal.
        led_a_reposo(audio_);
    }

private:
    void bucle() {
        auto t0 = std::chrono::steady_clock::now();
        while (corriendo_.load()) {
            auto ahora = std::chrono::steady_clock::now();
            double ms = std::chrono::duration<double, std::milli>(ahora - t0).count();

            // Seno desfasado para arrancar en oscuro y no con un fogonazo.
            double fase = 2.0 * M_PI * ms / LED_CICLO_MS;
            double nivel = (1.0 - std::cos(fase)) / 2.0;      // 0 -> 1 -> 0
            // Piso en 0.15: apagarse del todo en cada ciclo parece un
            // parpadeo defectuoso mas que una animacion.
            nivel = 0.15 + 0.85 * nivel;

            audio_->LedControl((uint8_t)(LED_R * nivel),
                               (uint8_t)(LED_G * nivel),
                               (uint8_t)(LED_B * nivel));

            // Se descuenta lo que tardo la llamada en vez de dormir el paso
            // entero. Si el RPC resulta mas lento que LED_PASO_MS, la animacion
            // se pone lenta sola en vez de encolar llamadas sin freno.
            auto gastado = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - ahora).count();
            double resta = LED_PASO_MS - gastado;
            if (resta > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds((int)resta));
        }
    }

    unitree::robot::g1::AudioClient* audio_;
    std::atomic<bool> corriendo_;
    std::thread hilo_;
};

#endif  // OTTO_LED_ANIM_HPP

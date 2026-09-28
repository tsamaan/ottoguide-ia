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
#include <thread>

#include <unitree/robot/g1/audio/g1_audio_client.hpp>

// Color base del pulso. Es el mismo verde-azulado que usa la web (--accent),
// para que el robot y la interfaz se lean como el mismo sistema.
#ifndef LED_R
#define LED_R 0
#define LED_G 180
#define LED_B 200
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
        // Apagar DESPUES del join: si se apagara antes, el hilo podria meter un
        // color mas entre medio y el LED quedaria prendido.
        audio_->LedControl(0, 0, 0);
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

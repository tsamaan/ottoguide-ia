#include <chrono>
#include <csignal>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <unistd.h>
#include <unitree/common/time/time_tool.hpp>
#include <unitree/robot/channel/channel_factory.hpp>
#include <unitree/robot/g1/audio/g1_audio_client.hpp>
#include "wav.hpp"
#include "led_anim.hpp"

#define CHUNK_SIZE 96000

// Para poder devolver el LED a su color de reposo si nos matan a mitad de un
// audio. Pasa de verdad: cancel_robot_voice() de app.py hace killall sobre este
// binario, y sin esto el LED quedaria congelado en un paso del pulso.
//
// El manejador solo escribe una bandera (lo unico seguro de hacer desde una
// senial); quien realmente apaga es el bucle de espera, que la mira cada 50ms y
// sale por el camino normal para que corra el destructor de LedHablando.
static volatile std::sig_atomic_t g_cortar = 0;
static void al_recibir_senial(int) { g_cortar = 1; }

int main(int argc, char const *argv[]) {
    if (argc < 3) {
        std::cerr << "Uso: otto_speak_file <interfaz> <archivo.wav> [volumen 0-100]" << std::endl;
        return 1;
    }

    // @INPUT: interfaz, wav, volumen opcional (default 70)
    const std::string net_iface = argv[1];
    const std::string wav_path  = argv[2];
    const uint8_t volume = (argc >= 4) ? (uint8_t)std::stoi(argv[3]) : 70;

    unitree::robot::ChannelFactory::Instance()->Init(0, net_iface);
    unitree::robot::g1::AudioClient client;
    client.SetTimeout(10.0f);
    client.Init();

    uint8_t vol = 0;
    int32_t ret = client.GetVolume(vol);
    if (ret != 0) {
        std::cerr << "[ERROR] GetVolume ret=" << ret << std::endl;
        return 1;
    }
    std::cout << "[OK] Conectado. Volumen actual: " << (int)vol << std::endl;

    client.SetVolume(volume);
    std::cout << "[INFO] Volumen fijado a " << (int)volume << std::endl;

    int32_t sr = -1; int8_t ch = 0; bool ok = false;
    auto pcm = ReadWave(wav_path, &sr, &ch, &ok);

    if (!ok || sr != 16000 || ch != 1) {
        std::cerr << "[ERROR] WAV invalido. Necesario: 16kHz mono 16-bit" << std::endl;
        return 1;
    }
    std::cout << "[OK] WAV: " << pcm.size() << " bytes | " << sr << "Hz | mono" << std::endl;

    std::signal(SIGINT, al_recibir_senial);
    std::signal(SIGTERM, al_recibir_senial);

    std::string sid = std::to_string(unitree::common::GetCurrentTimeMillisecond());
    size_t offset = 0, total = pcm.size();
    double dur = (double)total / (16000.0 * 2.0);
    std::cout << "[INFO] Reproduciendo stream=" << sid
              << " (" << dur << "s)" << std::endl;

    auto t_inicio = std::chrono::steady_clock::now();
    {
        // El LED pulsa SOLO mientras suena: se crea acá y el bloque cierra
        // justo cuando termina el audio. Este binario es el que usan la
        // botonera (otto_preset.sh) y las respuestas de GPT
        // (ask_gpt_and_speak.py), así que con esto quedan cubiertos esos dos
        // caminos. Al salir del bloque vuelve al color de reposo, incluso si
        // algo falla en el medio.
        LedHablando led(&client);

        while (offset < total && !g_cortar) {
            size_t sz = std::min((size_t)CHUNK_SIZE, total - offset);
            std::vector<uint8_t> chunk(pcm.begin() + offset, pcm.begin() + offset + sz);
            ret = client.PlayStream("otto", sid, chunk);
            std::cout << "[INFO] chunk=" << sz << " offset=" << offset << " ret=" << ret << std::endl;
            offset += sz;
            // Sólo ENTRE chunks: el Sleep(1) después del último era un segundo
            // entero de más, encima del +2 de abajo.
            if (offset < total) unitree::common::Sleep(1);
        }

        // Esperar lo que FALTA hasta que termine el audio, contando desde que
        // arrancó. Antes era `Sleep((int)dur + 2)` -- dos segundos fijos de más,
        // más el segundo del último chunk, más lo que se perdía por el (int).
        // Medido sobre un preset de 7.8s: 12s en total, 4.2s de sobra en los que
        // el LED seguía pulsando sin que sonara nada, y que además eran demora
        // real en cada toque de botonera. Es la misma cuenta que ya usaba
        // reproducir_wav() en otto_pipeline, que sí estaba bien.
        // El 0.3 es el colchón para que no se corte la cola del audio.
        while (!g_cortar) {
            double pasado = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - t_inicio).count();
            double resta = dur + 0.3 - pasado;
            if (resta <= 0) break;
            usleep((useconds_t)(std::min(resta, 0.05) * 1000000));
        }
    }  // <- acá el LED vuelve al reposo, justo al terminar el audio

    ret = client.PlayStop("otto");
    std::cout << "[OK] PlayStop ret=" << ret
              << (g_cortar ? " (cortado por senial)" : "") << std::endl;
    return 0;
}

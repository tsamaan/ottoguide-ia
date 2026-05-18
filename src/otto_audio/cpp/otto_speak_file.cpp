#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <unitree/common/time/time_tool.hpp>
#include <unitree/robot/channel/channel_factory.hpp>
#include <unitree/robot/g1/audio/g1_audio_client.hpp>
#include "wav.hpp"

#define CHUNK_SIZE 96000
#define SAFE_VOLUME 100

int main(int argc, char const *argv[]) {
    if (argc < 3) {
        std::cerr << "Uso: otto_speak_file <interfaz> <archivo.wav>" << std::endl;
        return 1;
    }
    unitree::robot::ChannelFactory::Instance()->Init(0, argv[1]);
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

    client.SetVolume(SAFE_VOLUME);
    std::cout << "[INFO] Volumen fijado a " << SAFE_VOLUME << std::endl;

    int32_t sr = -1; int8_t ch = 0; bool ok = false;
    auto pcm = ReadWave(argv[2], &sr, &ch, &ok);

    if (!ok || sr != 16000 || ch != 1) {
        std::cerr << "[ERROR] WAV invalido. Necesario: 16kHz mono 16-bit" << std::endl;
        return 1;
    }
    std::cout << "[OK] WAV: " << pcm.size() << " bytes | " << sr << "Hz | mono" << std::endl;

    std::string sid = std::to_string(unitree::common::GetCurrentTimeMillisecond());
    size_t offset = 0, total = pcm.size();
    std::cout << "[INFO] Reproduciendo stream=" << sid << std::endl;

    while (offset < total) {
        size_t sz = std::min((size_t)CHUNK_SIZE, total - offset);
        std::vector<uint8_t> chunk(pcm.begin() + offset, pcm.begin() + offset + sz);
        ret = client.PlayStream("otto", sid, chunk);
        std::cout << "[INFO] chunk=" << sz << " offset=" << offset << " ret=" << ret << std::endl;
        offset += sz;
        if (offset < total) unitree::common::Sleep(1);
    }

    double dur = (double)total / (16000.0 * 2.0);
    unitree::common::Sleep((int)dur + 2);
    ret = client.PlayStop("otto");
    std::cout << "[OK] PlayStop ret=" << ret << std::endl;
    return 0;
}

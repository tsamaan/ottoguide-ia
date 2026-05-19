#include <iostream>
#include <unistd.h>
#include <unitree/robot/channel/channel_factory.hpp>
#include <unitree/robot/channel/channel_subscriber.hpp>
#include <unitree/idl/ros2/String_.hpp>

void on_asr(const void* msg) {
    auto* m = (std_msgs::msg::dds_::String_*)msg;
    std::cout << "[ASR] " << m->data() << std::endl;
}

int main(int argc, char const *argv[]) {
    if (argc < 2) {
        std::cerr << "Uso: asr_test <interfaz>" << std::endl;
        return 1;
    }
    unitree::robot::ChannelFactory::Instance()->Init(0, argv[1]);
    unitree::robot::ChannelSubscriber<std_msgs::msg::dds_::String_> sub("rt/audio_msg");
    sub.InitChannel(on_asr);
    std::cout << "[INFO] Escuchando rt/audio_msg — hablá cerca del robot..." << std::endl;
    while (true) { sleep(1); }
    return 0;
}

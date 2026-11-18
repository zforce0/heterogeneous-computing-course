#pragma once

#include <atomic>
#include <thread>

int command_init();
void command_sel(int command_type);
bool update_fw_cmd(const char* file_path);

class CommandConsole {
public:
    CommandConsole();
    ~CommandConsole();

    bool start();
    void stop();

private:
    void run();

    std::atomic<bool> running_;
    std::thread thread_;
};

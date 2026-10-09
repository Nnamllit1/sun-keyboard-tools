#pragma once
#include "core.hpp"
#include "windows.hpp"
#include <atomic>
#include <mutex>
#include <thread>

namespace sun {
struct Status {
    std::wstring message{L"Starting..."}, lastKey{L"No extra key received yet"};
    uint64_t reports{}, presses{}, sent{}, skipped{};
    bool connected{};
};
class Backend {
    std::jthread thread_;
    std::mutex mutex_;
    Status status_;
    void run(std::stop_token stop, Profile profile, bool monitor);
public:
    ~Backend(){stop();}
    void start(Profile profile, bool monitor=false);
    void stop();
    Status status();
};
}

#pragma once
#include "protocol.hpp"
#include <stop_token>

namespace sun {
void captureKeyboard(std::stop_token stop,const Profile& profile,const std::function<void(wire::Frame)>& emit);
}

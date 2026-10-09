#pragma once
#include "security.hpp"
#include "protocol.hpp"
#include <stop_token>

namespace sun {
std::wstring pipeName();
Handle createCapturePipe();
bool pipeTransfer(HANDLE pipe,void* data,DWORD bytes,bool write,std::stop_token stop={},DWORD timeout=5000);
Handle connectCapture();
Handle authenticatePeer(HANDLE pipe,bool server);
void stopCapture();
}

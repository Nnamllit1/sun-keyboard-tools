#pragma once
#include <windows.h>
#include <filesystem>
#include <string>
#include <utility>

namespace sun {
struct Handle {
    HANDLE value{INVALID_HANDLE_VALUE};
    Handle()=default;
    explicit Handle(HANDLE h):value(h){}
    ~Handle(){reset();}
    Handle(const Handle&)=delete;
    Handle& operator=(const Handle&)=delete;
    Handle(Handle&& h) noexcept:value(std::exchange(h.value,INVALID_HANDLE_VALUE)){}
    Handle& operator=(Handle&& h) noexcept {reset(); value=std::exchange(h.value,INVALID_HANDLE_VALUE); return *this;}
    explicit operator bool() const {return value && value!=INVALID_HANDLE_VALUE;}
    void reset(){if (*this) CloseHandle(value); value=INVALID_HANDLE_VALUE;}
};
std::wstring wide(const std::string& text);
std::string utf8(const std::wstring& text);
std::filesystem::path executable();
std::filesystem::path userFolder();
std::wstring winError(DWORD code=GetLastError());
bool elevated();
void setStartup(bool enabled, const std::filesystem::path& exe);
bool hasStartup();
}

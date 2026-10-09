#pragma once
#include "windows.hpp"
#include <vector>

namespace sun {
void hardenProcess();
void dropCapturePrivileges();
std::filesystem::path installFolder();
std::wstring accountSid(HANDLE process=GetCurrentProcess());
std::wstring logonSid(HANDLE process=GetCurrentProcess());
bool processElevated(HANDLE process);
// Keeps every existing directory in a protected path open without delete sharing.
// Rejects reparses, untrusted owners and ACLs allowing unprivileged modification.
std::vector<Handle> lockProtected(const std::filesystem::path& path, bool directory=false);
void createProtectedDirectory(const std::filesystem::path& path);
void launchNormal(const std::filesystem::path& exe, const std::wstring& args, bool wait=false);
void requestCapture(HWND owner=nullptr);
void changeStartup(bool enabled, HWND owner);
bool runStartup();
}

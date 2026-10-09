#include "windows.hpp"
#include "security.hpp"
#include <shlobj.h>
#include <sddl.h>
#include <taskschd.h>
#include <wrl/client.h>
#include <comdef.h>
#include <stdexcept>
#include <vector>

namespace sun {
using Microsoft::WRL::ComPtr;
std::wstring wide(const std::string& text) {
    if (text.empty()) return {};
    auto n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0);
    if (!n) throw std::runtime_error("Text must be UTF-8");
    std::wstring result(n,0);
    MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),result.data(),n);
    return result;
}
std::string utf8(const std::wstring& text) {
    if (text.empty()) return {};
    auto n=WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr);
    std::string result(n,0);
    WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),result.data(),n,nullptr,nullptr);
    return result;
}
std::filesystem::path executable(){std::vector<wchar_t> s(32768); auto n=GetModuleFileNameW(nullptr,s.data(),static_cast<DWORD>(s.size())); if(!n||n==s.size())throw std::runtime_error("Cannot locate executable");return std::wstring(s.data(),n);}
std::filesystem::path userFolder() {
    PWSTR value{};
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&value))) throw std::runtime_error("Cannot locate application data");
    std::filesystem::path path(value); CoTaskMemFree(value); return path/L"SunKeyboardTools";
}
std::wstring winError(DWORD code) {
    wchar_t* text{};
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER|FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,code,0,reinterpret_cast<wchar_t*>(&text),0,nullptr);
    std::wstring result=text?text:L"Unknown Windows error"; LocalFree(text);
    while(!result.empty() && (result.back()=='\r'||result.back()=='\n'))result.pop_back();
    return result+L" ("+std::to_wstring(code)+L")";
}
bool elevated() {
    HANDLE raw{};
    if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&raw))return false;
    Handle token(raw); TOKEN_ELEVATION e{}; DWORD size{};
    return GetTokenInformation(token.value,TokenElevation,&e,sizeof(e),&size) && e.TokenIsElevated;
}
namespace {
void check(HRESULT hr){if(FAILED(hr))throw std::runtime_error("Task Scheduler: "+utf8(winError(static_cast<DWORD>(hr))));}
std::wstring sid() {
    HANDLE raw{};
    if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&raw))throw std::runtime_error("Cannot read account");
    Handle token(raw); DWORD size{};
    GetTokenInformation(token.value,TokenUser,nullptr,0,&size);
    std::vector<uint8_t> buffer(size);
    if(!GetTokenInformation(token.value,TokenUser,buffer.data(),size,&size))throw std::runtime_error("Cannot read account SID");
    LPWSTR value{};
    if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid,&value))throw std::runtime_error("Cannot format SID");
    std::wstring result(value);LocalFree(value);return result;
}
ComPtr<ITaskFolder> taskFolder(ComPtr<ITaskService>& service) {
    check(CoCreateInstance(CLSID_TaskScheduler,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&service)));
    check(service->Connect(_variant_t(),_variant_t(),_variant_t(),_variant_t()));
    ComPtr<ITaskFolder> folder;check(service->GetFolder(_bstr_t(L"\\"),&folder));return folder;
}
}
bool hasStartup() {
    ComPtr<ITaskService> service;
    auto folder=taskFolder(service);
    ComPtr<IRegisteredTask> task;
    auto name=L"SunKeyboardTools-"+sid();
    auto hr=folder->GetTask(_bstr_t(name.c_str()),&task);
    if(hr==HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))return false;
    check(hr); VARIANT_BOOL enabled{};check(task->get_Enabled(&enabled));return enabled!=VARIANT_FALSE;
}
void setStartup(bool enabled,const std::filesystem::path& exe) {
    if(!elevated())throw std::runtime_error("Changing privileged startup requires administrator permission");
    if(exe!=installFolder()/L"sun-keyboard-capture.exe")throw std::runtime_error("Startup requires the installed capture helper");
    std::vector<Handle> locks;if(enabled)locks=lockProtected(exe);
    ComPtr<ITaskService> service;
    auto folder=taskFolder(service);
    auto account=sid(), name=L"SunKeyboardTools-"+account;
    if(!enabled){auto hr=folder->DeleteTask(_bstr_t(name.c_str()),0);if(hr!=HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))check(hr);return;}
    ComPtr<ITaskDefinition> task;check(service->NewTask(0,&task));
    ComPtr<IRegistrationInfo> registration;check(task->get_RegistrationInfo(&registration));
    check(registration->put_Description(_bstr_t(L"Sun Keyboard Tools: enable Sun keyboard extra keys when this user signs in.")));
    ComPtr<IPrincipal> principal;check(task->get_Principal(&principal));
    check(principal->put_UserId(_bstr_t(account.c_str())));
    check(principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN));
    check(principal->put_RunLevel(TASK_RUNLEVEL_HIGHEST));
    ComPtr<ITaskSettings> settings;check(task->get_Settings(&settings));
    check(settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE));
    check(settings->put_StopIfGoingOnBatteries(VARIANT_FALSE));
    check(settings->put_ExecutionTimeLimit(_bstr_t(L"PT0S")));
    check(settings->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW));
    check(settings->put_StartWhenAvailable(VARIANT_TRUE));
    check(settings->put_RestartCount(3));check(settings->put_RestartInterval(_bstr_t(L"PT1M")));
    ComPtr<ITriggerCollection> triggers;check(task->get_Triggers(&triggers));
    ComPtr<ITrigger> trigger;check(triggers->Create(TASK_TRIGGER_LOGON,&trigger));
    ComPtr<ILogonTrigger> logon;check(trigger.As(&logon));
    check(logon->put_UserId(_bstr_t(account.c_str())));check(logon->put_Delay(_bstr_t(L"PT5S")));
    ComPtr<IActionCollection> actions;check(task->get_Actions(&actions));
    ComPtr<IAction> action;check(actions->Create(TASK_ACTION_EXEC,&action));
    ComPtr<IExecAction> exec;check(action.As(&exec));
    check(exec->put_Path(_bstr_t(exe.c_str())));check(exec->put_Arguments(_bstr_t(L"--background")));
    check(exec->put_WorkingDirectory(_bstr_t(exe.parent_path().c_str())));
    ComPtr<IRegisteredTask> registered;
    check(folder->RegisterTaskDefinition(_bstr_t(name.c_str()),task.Get(),TASK_CREATE_OR_UPDATE,
          _variant_t(account.c_str()),_variant_t(),TASK_LOGON_INTERACTIVE_TOKEN,
          _variant_t((L"O:BAG:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;GRGX;;;"+account+L")").c_str()),&registered));
}
bool runStartup() {
    try {
        ComPtr<ITaskService> service;auto folder=taskFolder(service);ComPtr<IRegisteredTask> task;
        check(folder->GetTask(_bstr_t((L"SunKeyboardTools-"+sid()).c_str()),&task));
        BSTR raw{};check(task->GetSecurityDescriptor(OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,&raw));
        _bstr_t descriptor(raw,false);PSECURITY_DESCRIPTOR sd{};
        if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(descriptor,SDDL_REVISION_1,&sd,nullptr))return false;
        struct Free {void* p;~Free(){LocalFree(p);}} cleanup{sd};
        PSID owner{};BOOL defaulted{},present{};PACL acl{};
        if(!GetSecurityDescriptorOwner(sd,&owner,&defaulted) || !owner || !IsWellKnownSid(owner,WinBuiltinAdministratorsSid) ||
           !GetSecurityDescriptorDacl(sd,&present,&acl,&defaulted) || !present || !acl)return false;
        for(DWORD i=0;i<acl->AceCount;++i) {
            void* rawAce{};if(!GetAce(acl,i,&rawAce))return false;auto header=static_cast<ACE_HEADER*>(rawAce);
            if(header->AceType==ACCESS_DENIED_ACE_TYPE)continue;
            if(header->AceType!=ACCESS_ALLOWED_ACE_TYPE)return false;
            auto ace=static_cast<ACCESS_ALLOWED_ACE*>(rawAce);auto trustee=&ace->SidStart;
            if(!IsWellKnownSid(trustee,WinBuiltinAdministratorsSid) && !IsWellKnownSid(trustee,WinLocalSystemSid) &&
               (ace->Mask&(GENERIC_ALL|GENERIC_WRITE|WRITE_DAC|WRITE_OWNER|DELETE|FILE_WRITE_DATA|FILE_APPEND_DATA|FILE_WRITE_EA|FILE_WRITE_ATTRIBUTES)))return false;
        }
        ComPtr<ITaskDefinition> definition;check(task->get_Definition(&definition));ComPtr<IActionCollection> actions;check(definition->get_Actions(&actions));
        LONG count{};check(actions->get_Count(&count));if(count!=1)return false;
        ComPtr<IAction> action;check(actions->get_Item(1,&action));ComPtr<IExecAction> exec;check(action.As(&exec));
        BSTR path{},args{};check(exec->get_Path(&path));_bstr_t exePath(path,false);check(exec->get_Arguments(&args));_bstr_t arguments(args,false);
        auto expected=installFolder()/L"sun-keyboard-capture.exe";
        if(_wcsicmp(exePath,expected.c_str())!=0 || wcscmp(arguments,L"--background")!=0)return false;
        auto locks=lockProtected(expected);ComPtr<IRunningTask> running;check(task->Run(_variant_t(),&running));return true;
    }catch(const std::exception&){return false;}
}
}

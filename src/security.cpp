#include "security.hpp"
#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <userenv.h>
#include <stdexcept>

namespace sun {
namespace {
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(std::string(message)+": "+utf8(winError()));}
Handle tokenFor(HANDLE process,DWORD rights=TOKEN_QUERY) {
    HANDLE raw{};require(OpenProcessToken(process,rights,&raw)!=FALSE,"Read process token");return Handle(raw);
}
std::vector<BYTE> tokenInfo(HANDLE token,TOKEN_INFORMATION_CLASS kind) {
    DWORD size{};GetTokenInformation(token,kind,nullptr,0,&size);
    if(!size || size>65536)throw std::runtime_error("Invalid token information size");
    std::vector<BYTE> bytes(size);require(GetTokenInformation(token,kind,bytes.data(),size,&size)!=FALSE,"Read token information");return bytes;
}
std::wstring sidText(PSID sid) {
    LPWSTR raw{};require(ConvertSidToStringSidW(sid,&raw)!=FALSE,"Format account SID");
    std::wstring result(raw);LocalFree(raw);return result;
}
bool trustedSid(PSID sid) {
    if(IsWellKnownSid(sid,WinLocalSystemSid) || IsWellKnownSid(sid,WinBuiltinAdministratorsSid))return true;
    return sidText(sid)==L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464"; // TrustedInstaller
}
void checkAcl(HANDLE file,bool leaf) {
    PSECURITY_DESCRIPTOR descriptor{};PSID owner{};PACL dacl{};
    auto code=GetSecurityInfo(file,SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,&owner,nullptr,&dacl,nullptr,&descriptor);
    if(code!=ERROR_SUCCESS)throw std::runtime_error("Cannot verify installation permissions");
    struct Free {void* p;~Free(){LocalFree(p);}} release{descriptor};
    if(!owner || !trustedSid(owner) || !dacl)throw std::runtime_error("Installation path must be owned and protected by administrators");
    constexpr DWORD alter=DELETE|WRITE_DAC|WRITE_OWNER|FILE_DELETE_CHILD|FILE_WRITE_DATA|FILE_WRITE_EA|FILE_WRITE_ATTRIBUTES;
    const DWORD forbidden=alter|(leaf?FILE_APPEND_DATA:0);
    for(DWORD i=0;i<dacl->AceCount;++i) {
        void* raw{};require(GetAce(dacl,i,&raw)!=FALSE,"Read installation permissions");
        auto header=static_cast<ACE_HEADER*>(raw);
        if(header->AceFlags&INHERIT_ONLY_ACE)continue;
        if(header->AceType==ACCESS_DENIED_ACE_TYPE || header->AceType==SYSTEM_MANDATORY_LABEL_ACE_TYPE)continue;
        // Unusual grant types need an explicit implementation, never a permissive fallback.
        if(header->AceType!=ACCESS_ALLOWED_ACE_TYPE)throw std::runtime_error("Unsupported installation access rule");
        auto ace=static_cast<ACCESS_ALLOWED_ACE*>(raw);DWORD mask=ace->Mask;
        GENERIC_MAPPING mapping{FILE_GENERIC_READ,FILE_GENERIC_WRITE,FILE_GENERIC_EXECUTE,FILE_ALL_ACCESS};MapGenericMask(&mask,&mapping);
        if((mask&forbidden) && !trustedSid(&ace->SidStart))throw std::runtime_error("Installation path is writable by an unprivileged account");
    }
}
void elevatedLaunch(const std::filesystem::path& exe,const wchar_t* args,HWND owner,bool wait) {
    auto locks=lockProtected(exe);
    SHELLEXECUTEINFOW info{sizeof(info)};info.fMask=SEE_MASK_NOCLOSEPROCESS|SEE_MASK_NOASYNC;
    info.hwnd=owner;info.lpVerb=L"runas";info.lpFile=exe.c_str();info.lpParameters=args;info.lpDirectory=exe.parent_path().c_str();info.nShow=SW_HIDE;
    require(ShellExecuteExW(&info)!=FALSE,"Administrator permission");Handle process(info.hProcess);
    if(wait){WaitForSingleObject(process.value,INFINITE);DWORD code{};require(GetExitCodeProcess(process.value,&code)!=FALSE && code==0,"Startup setting was not changed");}
}
}
void hardenProcess() {
    require(SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32)!=FALSE,"Restrict DLL search");
    SetDllDirectoryW(L"");
    PROCESS_MITIGATION_IMAGE_LOAD_POLICY image{};image.NoRemoteImages=1;image.NoLowMandatoryLabelImages=1;image.PreferSystem32Images=1;
    SetProcessMitigationPolicy(ProcessImageLoadPolicy,&image,sizeof(image));
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOOPENFILEERRORBOX);
}
void dropCapturePrivileges() {
    auto token=tokenFor(GetCurrentProcess(),TOKEN_QUERY|TOKEN_ADJUST_PRIVILEGES);
    auto bytes=tokenInfo(token.value,TokenPrivileges);auto privileges=reinterpret_cast<TOKEN_PRIVILEGES*>(bytes.data());
    LUID traverse{};require(LookupPrivilegeValueW(nullptr,SE_CHANGE_NOTIFY_NAME,&traverse)!=FALSE,"Find traverse privilege");
    for(DWORD i=0;i<privileges->PrivilegeCount;++i) {
        auto& privilege=privileges->Privileges[i];
        if(privilege.Luid.LowPart==traverse.LowPart && privilege.Luid.HighPart==traverse.HighPart)continue;
        privilege.Attributes=SE_PRIVILEGE_REMOVED;
    }
    SetLastError(ERROR_SUCCESS);
    require(AdjustTokenPrivileges(token.value,FALSE,privileges,0,nullptr,nullptr)!=FALSE && GetLastError()==ERROR_SUCCESS,"Remove unused capture privileges");
}
std::filesystem::path installFolder() {
    PWSTR raw{};require(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFiles,KF_FLAG_DEFAULT,nullptr,&raw)),"Locate Program Files");
    std::filesystem::path result(raw);CoTaskMemFree(raw);return result/L"Sun Keyboard Tools";
}
std::wstring accountSid(HANDLE process) {
    auto token=tokenFor(process);auto bytes=tokenInfo(token.value,TokenUser);return sidText(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid);
}
std::wstring logonSid(HANDLE process) {
    auto token=tokenFor(process);auto bytes=tokenInfo(token.value,TokenGroups);auto groups=reinterpret_cast<TOKEN_GROUPS*>(bytes.data());
    for(DWORD i=0;i<groups->GroupCount;++i)if((groups->Groups[i].Attributes&SE_GROUP_LOGON_ID)==SE_GROUP_LOGON_ID)return sidText(groups->Groups[i].Sid);
    throw std::runtime_error("An interactive Windows sign-in is required");
}
bool processElevated(HANDLE process) {
    auto token=tokenFor(process);TOKEN_ELEVATION e{};DWORD size{};require(GetTokenInformation(token.value,TokenElevation,&e,sizeof(e),&size)!=FALSE,"Read elevation");return e.TokenIsElevated!=0;
}
std::vector<Handle> lockProtected(const std::filesystem::path& path,bool directory) {
    if(!path.is_absolute() || path.wstring().starts_with(L"\\\\") || path!=path.lexically_normal())throw std::runtime_error("A local, absolute installation path is required");
    std::vector<Handle> locks;auto current=path.root_path();
    std::vector<std::filesystem::path> chain{current};
    for(const auto& part:path.relative_path()){current/=part;chain.push_back(current);}
    for(size_t i=0;i<chain.size();++i) {
        bool last=i+1==chain.size();
        Handle h(CreateFileW(chain[i].c_str(),READ_CONTROL|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr));
        require(static_cast<bool>(h),"Open protected installation path");BY_HANDLE_FILE_INFORMATION info{};
        require(GetFileInformationByHandle(h.value,&info)!=FALSE,"Inspect installation path");
        if(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)throw std::runtime_error("Reparse points are not allowed in privileged installation paths");
        if((!last || directory) && !(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY))throw std::runtime_error("Expected installation directory");
        if(last && !directory && ((info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) || info.nNumberOfLinks!=1))throw std::runtime_error("Expected a single-link installation file");
        try{checkAcl(h.value,last);}catch(const std::exception& e){throw std::runtime_error(std::string(e.what())+": "+utf8(chain[i].wstring()));}
        locks.push_back(std::move(h));
    }
    return locks;
}
void createProtectedDirectory(const std::filesystem::path& path) {
    if(std::filesystem::exists(path)){auto locks=lockProtected(path,true);return;}
    auto parent=lockProtected(path.parent_path(),true);
    PSECURITY_DESCRIPTOR sd{};
    require(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;GRGX;;;BU)",SDDL_REVISION_1,&sd,nullptr)!=FALSE,"Create installation permissions");
    SECURITY_ATTRIBUTES sa{sizeof(sa),sd,FALSE};auto ok=CreateDirectoryW(path.c_str(),&sa);auto error=GetLastError();LocalFree(sd);
    if(!ok && error!=ERROR_ALREADY_EXISTS)throw std::runtime_error("Cannot create protected installation directory");
    auto locks=lockProtected(path,true);
}
void launchNormal(const std::filesystem::path& exe,const std::wstring& args,bool wait) {
    // Some Windows configurations expose the linked token only at identification
    // level. Use the same sign-in's existing, non-elevated desktop token instead.
    DWORD pid{};GetWindowThreadProcessId(GetShellWindow(),&pid);
    Handle shell(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid));
    require(shell && !processElevated(shell.value) && accountSid(shell.value)==accountSid() && logonSid(shell.value)==logonSid(),
        "A normal Windows desktop in the same administrator account and sign-in is required");
    auto token=tokenFor(shell.value,TOKEN_QUERY|TOKEN_DUPLICATE);
    auto label=tokenInfo(token.value,TokenIntegrityLevel);auto sid=reinterpret_cast<TOKEN_MANDATORY_LABEL*>(label.data())->Label.Sid;
    auto rid=*GetSidSubAuthority(sid,*GetSidSubAuthorityCount(sid)-1);
    require(rid<=SECURITY_MANDATORY_MEDIUM_RID,"Refuse elevated desktop token");
    HANDLE primary{};require(DuplicateTokenEx(token.value,TOKEN_ALL_ACCESS,nullptr,SecurityImpersonation,TokenPrimary,&primary)!=FALSE,"Create normal user token");
    Handle normal(primary);
    void* environment{};require(CreateEnvironmentBlock(&environment,normal.value,FALSE)!=FALSE,"Create normal user environment");
    std::wstring command=L"\""+exe.wstring()+L"\" "+args;STARTUPINFOW startup{sizeof(startup)};PROCESS_INFORMATION process{};
    auto ok=CreateProcessWithTokenW(normal.value,0,exe.c_str(),command.data(),CREATE_UNICODE_ENVIRONMENT,environment,exe.parent_path().c_str(),&startup,&process);
    auto error=GetLastError();DestroyEnvironmentBlock(environment);
    if(!ok)throw std::runtime_error("Cannot start normal user UI: "+utf8(winError(error)));
    Handle child(process.hProcess),thread(process.hThread);
    if(wait) {
        if(WaitForSingleObject(child.value,30000)!=WAIT_OBJECT_0)throw std::runtime_error("Normal user setup step timed out");
        DWORD code{};if(!GetExitCodeProcess(child.value,&code) || code!=0)throw std::runtime_error("Normal user setup step failed");
    }
}
void requestCapture(HWND owner) {
    if(runStartup())return;
    elevatedLaunch(installFolder()/L"sun-keyboard-capture.exe",L"--capture",owner,false);
}
void changeStartup(bool enabled,HWND owner) {
    elevatedLaunch(installFolder()/L"Uninstall.exe",enabled?L"--startup-on --quiet":L"--startup-off --quiet",owner,true);
}
}

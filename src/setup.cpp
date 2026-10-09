#include "windows.hpp"
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <wintrust.h>
#include <softpub.h>
#include <wrl/client.h>
#include <fstream>
#include <vector>

#pragma comment(linker,"\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
namespace {
struct File {int resource; const wchar_t* path;};
constexpr File files[]{
    {100,L"sun-keyboard-tools.exe"},{101,L"profiles\\sun-type7.ini"},{102,L"README.html"},
    {103,L"COPYRIGHT.txt"},{104,L"third-party\\NOTICE.txt"},{105,L"third-party\\USBPcapSetup-1.5.4.0.exe"},
    {106,L"third-party\\usbpcap-src-incl-pdb-1.5.4.0.7z"},{107,L"third-party\\GPL-2.0.txt"},{108,L"third-party\\BSD-2-Clause.txt"}
};
constexpr wchar_t uninstallKey[]=L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\SunKeyboardTools";
std::filesystem::path known(REFKNOWNFOLDERID id){PWSTR value{};if(FAILED(SHGetKnownFolderPath(id,0,nullptr,&value)))throw std::runtime_error("Cannot locate Windows folder");std::filesystem::path p(value);CoTaskMemFree(value);return p;}
std::filesystem::path installFolder(){return known(FOLDERID_ProgramFiles)/L"Sun Keyboard Tools";}
void check(bool result,const char* operation){if(!result)throw std::runtime_error(std::string(operation)+": "+sun::utf8(sun::winError()));}
void noReparse(const std::filesystem::path& path){
    std::filesystem::path current;
    for(const auto& part:std::filesystem::absolute(path)){
        current/=part;auto attrs=GetFileAttributesW(current.c_str());
        if(attrs!=INVALID_FILE_ATTRIBUTES && (attrs&FILE_ATTRIBUTE_REPARSE_POINT))throw std::runtime_error("Setup will not write through a directory junction or symbolic link.");
    }
}
void extract(const std::filesystem::path& root){
    noReparse(root);
    std::filesystem::create_directories(root);
    for(auto file:files){
        auto path=root/file.path;noReparse(path);std::filesystem::create_directories(path.parent_path());
        auto resource=FindResourceW(nullptr,MAKEINTRESOURCEW(file.resource),RT_RCDATA);
        if(!resource)throw std::runtime_error("Setup resource is missing");
        auto data=LoadResource(nullptr,resource);auto size=SizeofResource(nullptr,resource);auto bytes=LockResource(data);
        auto temp=path;temp+=L".new";noReparse(temp);
        sun::Handle output(CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
        if(!output)throw std::runtime_error("Cannot stage setup file: "+sun::utf8(sun::winError()));
        DWORD written{};bool ok=WriteFile(output.value,bytes,size,&written,nullptr) && written==size && FlushFileBuffers(output.value);
        output.reset();if(!ok){DeleteFileW(temp.c_str());throw std::runtime_error("Cannot write setup file");}
        if(!MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){auto err=sun::winError();DeleteFileW(temp.c_str());throw std::runtime_error("Cannot install file: "+sun::utf8(err));}
    }
}
void trust(const std::filesystem::path& exe){
    WINTRUST_FILE_INFO file{sizeof(file)};file.pcwszFilePath=exe.c_str();
    WINTRUST_DATA data{sizeof(data)};data.dwUIChoice=WTD_UI_NONE;data.fdwRevocationChecks=WTD_REVOKE_NONE;
    data.dwUnionChoice=WTD_CHOICE_FILE;data.pFile=&file;data.dwStateAction=WTD_STATEACTION_VERIFY;
    data.dwProvFlags=WTD_CACHE_ONLY_URL_RETRIEVAL;
    GUID policy=WINTRUST_ACTION_GENERIC_VERIFY_V2;
    auto status=WinVerifyTrust(nullptr,&policy,&data);data.dwStateAction=WTD_STATEACTION_CLOSE;WinVerifyTrust(nullptr,&policy,&data);
    if(status!=ERROR_SUCCESS)throw std::runtime_error("The bundled USBPcap Authenticode signature could not be verified. Installation stopped.");
}
DWORD run(const std::filesystem::path& exe,const std::wstring& args){
    std::wstring command=L"\""+exe.wstring()+L"\" "+args;
    STARTUPINFOW si{sizeof(si)};si.dwFlags=STARTF_USESHOWWINDOW;si.wShowWindow=SW_HIDE;PROCESS_INFORMATION pi{};
    check(CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,exe.parent_path().c_str(),&si,&pi)!=FALSE,"Start USBPcap setup");
    sun::Handle process(pi.hProcess),thread(pi.hThread);
    WaitForSingleObject(process.value,INFINITE);DWORD code{};check(GetExitCodeProcess(process.value,&code)!=FALSE,"Read setup result");return code;
}
bool driverInstalled(){
    auto manager=OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT);
    if(!manager)throw std::runtime_error("Cannot query driver service");
    auto service=OpenServiceW(manager,L"USBPcap",SERVICE_QUERY_STATUS);auto error=GetLastError();
    if(service)CloseServiceHandle(service);CloseServiceHandle(manager);
    if(service)return true;
    if(error!=ERROR_SERVICE_DOES_NOT_EXIST)throw std::runtime_error("USBPcap service is unavailable: "+sun::utf8(sun::winError(error)));
    HKEY key{};auto reg=RegOpenKeyExW(HKEY_LOCAL_MACHINE,L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\USBPcap",0,KEY_READ|KEY_WOW64_64KEY,&key);
    if(reg==ERROR_SUCCESS){RegCloseKey(key);throw std::runtime_error("USBPcap has an existing installer entry but no driver service. Repair or uninstall that installation first, then restart Windows.");}
    return false;
}
void writeString(HKEY key,const wchar_t* name,const std::wstring& value){
    auto result=RegSetValueExW(key,name,0,REG_SZ,reinterpret_cast<const BYTE*>(value.c_str()),static_cast<DWORD>((value.size()+1)*sizeof(wchar_t)));
    if(result!=ERROR_SUCCESS)throw std::runtime_error("Cannot register Installed apps entry: "+sun::utf8(sun::winError(result)));
}
void shortcut(const std::filesystem::path& root){
    Microsoft::WRL::ComPtr<IShellLinkW> link;
    if(FAILED(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&link))))throw std::runtime_error("Cannot create Start menu shortcut");
    auto exe=root/L"sun-keyboard-tools.exe";
    link->SetPath(exe.c_str());link->SetWorkingDirectory(root.c_str());link->SetDescription(L"Remap the extra keys on your Sun keyboard");link->SetIconLocation(exe.c_str(),0);
    Microsoft::WRL::ComPtr<IPersistFile> file;link.As(&file);
    auto path=known(FOLDERID_CommonPrograms)/L"Sun Keyboard Tools.lnk";
    if(FAILED(file->Save(path.c_str(),TRUE)))throw std::runtime_error("Cannot save Start menu shortcut");
}
void registerApp(const std::filesystem::path& root){
    HKEY key{};auto result=RegCreateKeyExW(HKEY_LOCAL_MACHINE,uninstallKey,0,nullptr,0,KEY_WRITE|KEY_WOW64_64KEY,nullptr,&key,nullptr);
    if(result!=ERROR_SUCCESS)throw std::runtime_error("Cannot create Installed apps entry");
    try{
        writeString(key,L"DisplayName",L"Sun Keyboard Tools");writeString(key,L"DisplayVersion",L"0.1.0");
        writeString(key,L"Publisher",L"Sun Keyboard Tools contributors");writeString(key,L"InstallLocation",root.wstring());
        writeString(key,L"DisplayIcon",(root/L"sun-keyboard-tools.exe").wstring());
        writeString(key,L"UninstallString",L"\""+(root/L"Uninstall.exe").wstring()+L"\" --uninstall");
        DWORD one=1;RegSetValueExW(key,L"NoModify",0,REG_DWORD,reinterpret_cast<BYTE*>(&one),sizeof(one));RegSetValueExW(key,L"NoRepair",0,REG_DWORD,reinterpret_cast<BYTE*>(&one),sizeof(one));
    }catch(...){RegCloseKey(key);throw;}RegCloseKey(key);
}
void requireAppStopped(){
    sun::Handle mutex(OpenMutexW(SYNCHRONIZE,FALSE,L"Local\\SunKeyboardTools.App"));
    if(mutex)throw std::runtime_error("Exit Sun Keyboard Tools from its tray menu before installing, updating, or uninstalling.");
}
bool install(){
    requireAppStopped();auto root=installFolder();noReparse(root);
    // Check before modifying any app files so a pending driver removal is actionable.
    bool hadDriver=driverInstalled();extract(root);
    check(CopyFileW(sun::executable().c_str(),(root/L"Uninstall.exe").c_str(),FALSE)!=FALSE,"Install uninstaller");
    // Register early: a partially completed install can still be removed normally.
    registerApp(root);
    if(!hadDriver){
        auto driver=root/L"third-party"/L"USBPcapSetup-1.5.4.0.exe";trust(driver);
        auto result=run(driver,L"/S");
        if(result!=0 && result!=3010)throw std::runtime_error("USBPcap installer returned "+std::to_string(result));
        if(!driverInstalled())throw std::runtime_error("USBPcap setup did not register the driver. Review the Windows installation log.");
    }
    shortcut(root);sun::setStartup(true,root/L"sun-keyboard-tools.exe");return !hadDriver;
}
void uninstall(){
    requireAppStopped();auto root=installFolder();noReparse(root);
    sun::setStartup(false,root/L"sun-keyboard-tools.exe");
    std::error_code error;
    std::filesystem::remove(known(FOLDERID_CommonPrograms)/L"Sun Keyboard Tools.lnk",error);
    for(auto file:files){auto path=root/file.path;noReparse(path);std::filesystem::remove(path,error);if(error)throw std::runtime_error("Could not remove "+sun::utf8(path.wstring())+": "+error.message());}
    std::filesystem::remove(root/L"profiles",error);std::filesystem::remove(root/L"third-party",error);
    // Windows removes the running uninstaller at the next restart. No shell deletion.
    if(sun::executable()==root/L"Uninstall.exe"){
        check(MoveFileExW(sun::executable().c_str(),nullptr,MOVEFILE_DELAY_UNTIL_REBOOT)!=FALSE,"Schedule uninstaller cleanup");
        MoveFileExW(root.c_str(),nullptr,MOVEFILE_DELAY_UNTIL_REBOOT);
    }else{std::filesystem::remove(root/L"Uninstall.exe",error);std::filesystem::remove(root,error);}
    auto reg=RegDeleteKeyExW(HKEY_LOCAL_MACHINE,uninstallKey,KEY_WOW64_64KEY,0);
    if(reg!=ERROR_SUCCESS && reg!=ERROR_FILE_NOT_FOUND)throw std::runtime_error("Cannot remove Installed apps entry");
}
int elevate(const wchar_t* args){
    auto exe=sun::executable();SHELLEXECUTEINFOW info{sizeof(info)};
    info.fMask=SEE_MASK_NOCLOSEPROCESS;info.lpVerb=L"runas";info.lpFile=exe.c_str();info.lpParameters=args;info.nShow=SW_HIDE;
    check(ShellExecuteExW(&info)!=FALSE,"Administrator permission");sun::Handle process(info.hProcess);WaitForSingleObject(process.value,INFINITE);DWORD code{};GetExitCodeProcess(process.value,&code);return static_cast<int>(code);
}
}
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int){
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32|LOAD_LIBRARY_SEARCH_APPLICATION_DIR);
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    bool quiet=false;
    try{
        int argc{};auto raw=CommandLineToArgvW(GetCommandLineW(),&argc);std::vector<std::wstring> args(raw,raw+argc);LocalFree(raw);
        if(argc==3 && args[1]==L"--extract"){
            auto root=std::filesystem::absolute(args[2]);if(std::filesystem::exists(root))throw std::runtime_error("Extraction destination must not already exist");extract(root);trust(root/L"third-party"/L"USBPcapSetup-1.5.4.0.exe");return 0;
        }
        bool remove=argc>1 && args[1]==L"--uninstall";
        bool direct=argc>1 && (args[1]==L"--install" || args[1]==L"--remove");
        quiet=argc>2 && args[2]==L"--quiet";
        if(direct){
            if(!sun::elevated())throw std::runtime_error("The install/remove command requires administrator privileges");
            bool reboot=false;if(args[1]==L"--remove")uninstall();else reboot=install();
            if(!quiet)MessageBoxW(nullptr,args[1]==L"--remove"?L"Sun Keyboard Tools was removed. Your profiles and the shared USBPcap driver were kept. The uninstaller is cleaned up at the next Windows restart.":
                reboot?L"Installed. Restart Windows to activate USBPcap. Sun Keyboard Tools will start automatically when you sign in. Setup will not restart the computer for you.":
                L"Installed. Open Sun Keyboard Tools from Start. Automatic startup is enabled for this account.",L"Sun Keyboard Tools",MB_OK|MB_ICONINFORMATION);
            return reboot?3010:0;
        }
        INITCOMMONCONTROLSEX cc{sizeof(cc),ICC_STANDARD_CLASSES};InitCommonControlsEx(&cc);
        TASKDIALOGCONFIG dialog{sizeof(dialog)};dialog.dwFlags=TDF_SIZE_TO_CONTENT;dialog.pszWindowTitle=L"Sun Keyboard Tools Setup";
        dialog.pszMainIcon=TD_INFORMATION_ICON;
        dialog.pszMainInstruction=remove?L"Remove Sun Keyboard Tools?":L"Make your Sun keyboard's extra keys useful again.";
        dialog.pszContent=remove?L"Removes the application and this account's automatic startup task. Saved profiles and the shared USBPcap driver are kept. Exit the app from its tray menu first.":
            L"Installs the C++ remapper, Start menu shortcut, and automatic startup for this administrator account.\n\nIncludes the signed, open-source USBPcap 1.5.4.0 USB filter driver if it is not already installed. A Windows restart is normally needed. No automatic restart.\n\nWindows 11 compatibility is experimental: an unresolved upstream USBPcap crash report exists (issue #146). Do not disable Windows driver security.\n\nUSBPcap's matching source and GPL/BSD license texts are included. The original app code has no license selected yet.\n\nInstall location: Program Files\\Sun Keyboard Tools";
        TASKDIALOG_BUTTON buttons[]{{IDOK,remove?L"Uninstall":L"Install"},{IDCANCEL,L"Cancel"}};dialog.cButtons=2;dialog.pButtons=buttons;dialog.nDefaultButton=IDOK;
        int clicked{};if(FAILED(TaskDialogIndirect(&dialog,&clicked,nullptr,nullptr)))throw std::runtime_error("Cannot open setup dialog");
        if(clicked!=IDOK)return 0;
        return elevate(remove?L"--remove":L"--install");
    }catch(const std::exception& e){
        if(quiet){std::ofstream out(sun::executable().parent_path()/L"setup-error.txt");out<<e.what();}
        else MessageBoxW(nullptr,sun::wide(e.what()).c_str(),L"Sun Keyboard Tools Setup",MB_OK|MB_ICONERROR);
        return 1;
    }
}

#include "windows.hpp"
#include "maintenance.hpp"
#include "version.hpp"
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <wintrust.h>
#include <softpub.h>
#include <wrl/client.h>
#include <atomic>
#include <fstream>
#include <functional>
#include <thread>
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
constexpr wchar_t usbClassKey[]=L"SYSTEM\\CurrentControlSet\\Control\\Class\\{36FC9E60-C465-11CF-8056-444553540000}";
constexpr wchar_t resumeKey[]=L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce";
using Progress=std::function<void(const std::wstring&)>;

enum class Operation {install, remove, removeAll, driverInstall, driverReinstall};
struct Result { bool reboot{}; std::wstring message; bool launch{}; };
std::filesystem::path known(REFKNOWNFOLDERID id) {
    PWSTR value{};
    if(FAILED(SHGetKnownFolderPath(id,0,nullptr,&value)))throw std::runtime_error("Cannot locate Windows folder");
    std::filesystem::path path(value);CoTaskMemFree(value);return path;
}
std::filesystem::path installFolder(){return known(FOLDERID_ProgramFiles)/L"Sun Keyboard Tools";}
std::filesystem::path driverFolder(){return known(FOLDERID_ProgramFiles)/L"USBPcap";}
void check(bool result,const char* operation){if(!result)throw std::runtime_error(std::string(operation)+": "+sun::utf8(sun::winError()));}
void noReparse(const std::filesystem::path& path) {
    std::filesystem::path current;
    for(const auto& part:std::filesystem::absolute(path)) {
        current/=part;auto attrs=GetFileAttributesW(current.c_str());
        if(attrs!=INVALID_FILE_ATTRIBUTES && (attrs&FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("Setup will not write through a directory junction or symbolic link.");
    }
}
void extract(const std::filesystem::path& root) {
    noReparse(root);std::filesystem::create_directories(root);
    for(auto file:files) {
        auto path=root/file.path;noReparse(path);std::filesystem::create_directories(path.parent_path());
        auto resource=FindResourceW(nullptr,MAKEINTRESOURCEW(file.resource),RT_RCDATA);
        if(!resource)throw std::runtime_error("Setup resource is missing");
        auto data=LoadResource(nullptr,resource);auto size=SizeofResource(nullptr,resource);auto bytes=LockResource(data);
        auto temp=path;temp+=L".new-"+std::to_wstring(GetCurrentProcessId());noReparse(temp);
        sun::Handle output(CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
        if(!output)throw std::runtime_error("Cannot stage setup file: "+sun::utf8(sun::winError()));
        DWORD written{};
        bool ok=WriteFile(output.value,bytes,size,&written,nullptr) && written==size && FlushFileBuffers(output.value);
        output.reset();if(!ok){DeleteFileW(temp.c_str());throw std::runtime_error("Cannot write setup file");}
        if(!MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) {
            auto err=sun::winError();DeleteFileW(temp.c_str());throw std::runtime_error("Cannot install file: "+sun::utf8(err));
        }
    }
}
void trust(const std::filesystem::path& exe) {
    WINTRUST_FILE_INFO file{sizeof(file)};file.pcwszFilePath=exe.c_str();
    WINTRUST_DATA data{sizeof(data)};data.dwUIChoice=WTD_UI_NONE;data.fdwRevocationChecks=WTD_REVOKE_NONE;
    data.dwUnionChoice=WTD_CHOICE_FILE;data.pFile=&file;data.dwStateAction=WTD_STATEACTION_VERIFY;
    data.dwProvFlags=WTD_CACHE_ONLY_URL_RETRIEVAL;
    GUID policy=WINTRUST_ACTION_GENERIC_VERIFY_V2;
    auto status=WinVerifyTrust(nullptr,&policy,&data);
    data.dwStateAction=WTD_STATEACTION_CLOSE;WinVerifyTrust(nullptr,&policy,&data);
    if(status!=ERROR_SUCCESS)throw std::runtime_error("USBPcap's digital signature could not be verified. No driver operation was started.");
}
DWORD run(const std::filesystem::path& exe,const std::wstring& args) {
    std::wstring command=L"\""+exe.wstring()+L"\" "+args;
    STARTUPINFOW si{sizeof(si)};si.dwFlags=STARTF_USESHOWWINDOW;si.wShowWindow=SW_HIDE;PROCESS_INFORMATION pi{};
    check(CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,exe.parent_path().c_str(),&si,&pi)!=FALSE,"Start USBPcap maintenance");
    sun::Handle process(pi.hProcess),thread(pi.hThread);
    WaitForSingleObject(process.value,INFINITE);DWORD code{};check(GetExitCodeProcess(process.value,&code)!=FALSE,"Read maintenance result");return code;
}
sun::DriverFacts driverFacts() {
    sun::DriverFacts facts;
    auto manager=OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT);
    if(!manager)throw std::runtime_error("Cannot query driver service");
    auto service=OpenServiceW(manager,L"USBPcap",SERVICE_QUERY_STATUS);auto error=GetLastError();
    if(service) {
        facts.service=true;SERVICE_STATUS status{};
        if(QueryServiceStatus(service,&status))facts.running=status.dwCurrentState==SERVICE_RUNNING;
        CloseServiceHandle(service);
    } else if(error==ERROR_SERVICE_MARKED_FOR_DELETE)facts.pendingDelete=true;
    else if(error!=ERROR_SERVICE_DOES_NOT_EXIST) {
        CloseServiceHandle(manager);throw std::runtime_error("Cannot read USBPcap status: "+sun::utf8(sun::winError(error)));
    }
    CloseServiceHandle(manager);
    HKEY registration{};
    if(RegOpenKeyExW(HKEY_LOCAL_MACHINE,L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\USBPcap",0,KEY_READ|KEY_WOW64_64KEY,&registration)==ERROR_SUCCESS) {
        facts.registered=true;RegCloseKey(registration);
    }
    DWORD pending{},size=sizeof(pending);
    if(RegGetValueW(HKEY_LOCAL_MACHINE,L"SYSTEM\\CurrentControlSet\\Services\\USBPcap",L"DeleteFlag",RRF_RT_REG_DWORD,nullptr,&pending,&size)==ERROR_SUCCESS)
        facts.pendingDelete=pending!=0;
    std::vector<wchar_t> filters(32768);size=static_cast<DWORD>(filters.size()*sizeof(wchar_t));
    if(RegGetValueW(HKEY_LOCAL_MACHINE,usbClassKey,L"UpperFilters",RRF_RT_REG_MULTI_SZ,nullptr,filters.data(),&size)==ERROR_SUCCESS)
        for(auto p=filters.data();*p;p+=wcslen(p)+1)if(_wcsicmp(p,L"USBPcap")==0)facts.filter=true;
    facts.binary=std::filesystem::exists(known(FOLDERID_System)/L"drivers"/L"USBPcap.sys");
    std::vector<wchar_t> names(65536);
    if(QueryDosDeviceW(nullptr,names.data(),static_cast<DWORD>(names.size())))
        for(auto p=names.data();*p;p+=wcslen(p)+1)if(std::wstring_view(p).starts_with(L"USBPcap"))facts.capture=true;
    return facts;
}
std::wstring driverDescription(const sun::DriverFacts& facts) {
    switch(sun::driverPhase(facts)) {
    case sun::DriverPhase::active:return L"USBPcap is installed and running.";
    case sun::DriverPhase::restart:return L"USBPcap needs a Windows restart before the next driver step.";
    case sun::DriverPhase::incomplete:return L"USBPcap installation is incomplete. Use Reinstall driver or Windows Installed apps.";
    default:return L"USBPcap is not installed.";
    }
}
void writeString(HKEY key,const wchar_t* name,const std::wstring& value) {
    auto result=RegSetValueExW(key,name,0,REG_SZ,reinterpret_cast<const BYTE*>(value.c_str()),static_cast<DWORD>((value.size()+1)*sizeof(wchar_t)));
    if(result!=ERROR_SUCCESS)throw std::runtime_error("Cannot save installation setting: "+sun::utf8(sun::winError(result)));
}
void resumeAfterSignIn(bool enable) {
    HKEY key{};
    auto result=RegCreateKeyExW(HKEY_CURRENT_USER,resumeKey,0,nullptr,0,KEY_SET_VALUE,nullptr,&key,nullptr);
    if(result!=ERROR_SUCCESS)throw std::runtime_error("Cannot configure driver setup reminder");
    try {
        if(enable)writeString(key,L"SunKeyboardToolsDriverSetup",L"\""+(installFolder()/L"Uninstall.exe").wstring()+L"\" --driver");
        else RegDeleteValueW(key,L"SunKeyboardToolsDriverSetup");
    } catch(...) {RegCloseKey(key);throw;}
    RegCloseKey(key);
}
void stopApp() {
    auto window=FindWindowW(L"SunKeyboardTools.Settings",nullptr);
    if(window) {
        DWORD_PTR result{};
        // Existing 0.1.0 and newer apps both handle Exit as command 108.
        if(!SendMessageTimeoutW(window,WM_COMMAND,108,0,SMTO_ABORTIFHUNG|SMTO_BLOCK,30000,&result))
            throw std::runtime_error("The remapper did not close. Save any pending shortcut edits and choose Exit from its tray menu, then retry.");
    }
    for(int i=0;i<50;++i) {
        sun::Handle mutex(OpenMutexW(SYNCHRONIZE,FALSE,L"Local\\SunKeyboardTools.App"));
        if(!mutex)return;Sleep(100);
    }
    throw std::runtime_error("The remapper is still running. Choose Exit from its tray menu and retry.");
}
void makeShortcut(const std::wstring& name,const std::filesystem::path& exe,const wchar_t* args) {
    Microsoft::WRL::ComPtr<IShellLinkW> link;
    if(FAILED(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&link))))throw std::runtime_error("Cannot create Start menu shortcut");
    link->SetPath(exe.c_str());link->SetArguments(args);link->SetWorkingDirectory(exe.parent_path().c_str());
    link->SetDescription(L"Sun keyboard remapping and setup");link->SetIconLocation(exe.c_str(),0);
    Microsoft::WRL::ComPtr<IPersistFile> file;link.As(&file);
    auto path=known(FOLDERID_CommonPrograms)/(name+L".lnk");
    if(FAILED(file->Save(path.c_str(),TRUE)))throw std::runtime_error("Cannot save Start menu shortcut");
}
void registerApp(const std::filesystem::path& root) {
    HKEY key{};
    auto result=RegCreateKeyExW(HKEY_LOCAL_MACHINE,uninstallKey,0,nullptr,0,KEY_WRITE|KEY_WOW64_64KEY,nullptr,&key,nullptr);
    if(result!=ERROR_SUCCESS)throw std::runtime_error("Cannot create Installed apps entry");
    try {
        writeString(key,L"DisplayName",L"Sun Keyboard Tools");writeString(key,L"DisplayVersion",sun::wideVersion);
        writeString(key,L"Publisher",L"Sun Keyboard Tools contributors");writeString(key,L"InstallLocation",root.wstring());
        writeString(key,L"DisplayIcon",(root/L"sun-keyboard-tools.exe").wstring());
        auto setup=L"\""+(root/L"Uninstall.exe").wstring()+L"\"";
        writeString(key,L"UninstallString",setup+L" --uninstall");writeString(key,L"ModifyPath",setup+L" --manage");
        writeString(key,L"URLInfoAbout",L"https://github.com/Nnamllit1/sun-keyboard-tools");
        RegDeleteValueW(key,L"NoModify");DWORD one=1;
        RegSetValueExW(key,L"NoRepair",0,REG_DWORD,reinterpret_cast<BYTE*>(&one),sizeof(one));
    } catch(...) {RegCloseKey(key);throw;}
    RegCloseKey(key);
}
void installDriver(const Progress& progress) {
    auto facts=driverFacts();
    if(!sun::mayInstallDriver(facts))throw std::runtime_error(sun::utf8(driverDescription(facts))+" Setup will not overwrite a loaded or pending driver. Restart if requested, or use Reinstall driver.");
    auto driver=installFolder()/L"third-party"/L"USBPcapSetup-1.5.4.0.exe";
    progress(L"Verifying and installing the signed USBPcap driver...");trust(driver);
    auto result=run(driver,L"/S");
    if(result!=0 && result!=3010)throw std::runtime_error("USBPcap installer returned "+std::to_string(result));
    auto after=driverFacts();
    if(!after.service || !after.filter || !after.binary)throw std::runtime_error("USBPcap setup did not complete. Reopen Setup to review driver status.");
    resumeAfterSignIn(false);
}
void removeDriver(const Progress& progress) {
    auto uninstaller=driverFolder()/L"Uninstall.exe";noReparse(uninstaller);
    if(!std::filesystem::exists(uninstaller))throw std::runtime_error("The USBPcap uninstaller was not found in Program Files. Remove USBPcap through Windows Installed apps, restart, then reopen Sun Keyboard Tools Setup.");
    trust(uninstaller);progress(L"Removing the shared USBPcap driver. Please wait...");
    // NSIS _?= keeps this process as the uninstaller so waiting is meaningful.
    auto result=run(uninstaller,L"/S _?="+driverFolder().wstring());
    if(result!=0 && result!=3010)throw std::runtime_error("USBPcap uninstaller returned "+std::to_string(result));
    if(driverFacts().filter)throw std::runtime_error("USBPcap still appears in the USB filter list. Check Windows Installed apps before continuing.");
}
Result install(const Progress& progress) {
    auto root=installFolder();noReparse(root);
    bool existed=std::filesystem::exists(root/L"sun-keyboard-tools.exe");
    bool startup=existed?sun::hasStartup():true;
    auto facts=driverFacts();
    progress(L"Closing the remapper safely...");stopApp();
    progress(L"Installing application files and keyboard profiles...");extract(root);
    auto own=sun::executable();auto maintenance=root/L"Uninstall.exe";
    noReparse(maintenance);
    if(!std::filesystem::equivalent(own,root/L"sun-keyboard-tools.exe") && own!=maintenance)
        check(CopyFileW(own.c_str(),maintenance.c_str(),FALSE)!=FALSE,"Install maintenance wizard");
    registerApp(root);
    bool reboot=false;
    if(sun::mayInstallDriver(facts)){installDriver(progress);reboot=true;}
    else reboot=sun::driverPhase(facts)!=sun::DriverPhase::active;
    progress(L"Adding Start menu shortcuts and restoring your startup preference...");
    makeShortcut(L"Sun Keyboard Tools",root/L"sun-keyboard-tools.exe",L"");
    makeShortcut(L"Sun Keyboard Tools Setup",maintenance,L"--manage");
    sun::setStartup(startup,root/L"sun-keyboard-tools.exe");
    if(sun::driverPhase(driverFacts())==sun::DriverPhase::incomplete)
        return {false,L"The app is installed, but USBPcap needs attention. Choose Driver options in Setup. Your profiles and startup preference were preserved.",false};
    return {reboot,reboot?L"The app is installed. Restart Windows when you are ready, then open Sun Keyboard Tools. Setup will not restart Windows automatically.":
        L"The app is ready. Your existing profiles and startup preference were preserved. Open Sun Keyboard Tools to continue.",!reboot};
}
void deleteFile(const std::filesystem::path& path) {
    noReparse(path);std::error_code error;std::filesystem::remove(path,error);
    if(error)throw std::runtime_error("Could not remove "+sun::utf8(path.wstring())+": "+error.message());
}
void uninstall(const Progress& progress) {
    auto root=installFolder();noReparse(root);stopApp();
    progress(L"Removing the app and this account's startup task...");
    sun::setStartup(false,root/L"sun-keyboard-tools.exe");resumeAfterSignIn(false);
    for(auto name:{L"Sun Keyboard Tools.lnk",L"Sun Keyboard Tools Setup.lnk"})deleteFile(known(FOLDERID_CommonPrograms)/name);
    // Rename a running uninstaller to a unique protected path. Never schedule
    // deletion of the normal install path: a reinstall before reboot must survive.
    auto maintenance=root/L"Uninstall.exe";
    if(std::filesystem::exists(maintenance)) {
        if(sun::executable()==maintenance) {
            GUID guid{};check(SUCCEEDED(CoCreateGuid(&guid)),"Create cleanup ID");wchar_t text[40]{};StringFromGUID2(guid,text,40);
            auto cleanup=known(FOLDERID_ProgramFiles)/L"Sun Keyboard Tools Cleanup"/text;
            noReparse(cleanup);std::filesystem::create_directories(cleanup);
            auto moved=cleanup/L"Uninstall.exe";
            check(MoveFileExW(maintenance.c_str(),moved.c_str(),MOVEFILE_WRITE_THROUGH)!=FALSE,"Prepare uninstaller cleanup");
            check(MoveFileExW(moved.c_str(),nullptr,MOVEFILE_DELAY_UNTIL_REBOOT)!=FALSE,"Schedule uninstaller cleanup");
            MoveFileExW(cleanup.c_str(),nullptr,MOVEFILE_DELAY_UNTIL_REBOOT);
        } else deleteFile(maintenance);
    }
    for(auto file:files)deleteFile(root/file.path);
    std::error_code ignored;
    std::filesystem::remove(root/L"profiles",ignored);std::filesystem::remove(root/L"third-party",ignored);
    std::filesystem::remove(root,ignored); // Leave any unknown/user-added files.
    auto reg=RegDeleteKeyExW(HKEY_LOCAL_MACHINE,uninstallKey,KEY_WOW64_64KEY,0);
    if(reg!=ERROR_SUCCESS && reg!=ERROR_FILE_NOT_FOUND)throw std::runtime_error("Cannot remove Installed apps entry");
}
Result perform(Operation operation,const Progress& progress) {
    switch(operation) {
    case Operation::install:return install(progress);
    case Operation::remove:uninstall(progress);return {false,L"Sun Keyboard Tools was removed. Saved profiles and the shared USBPcap driver were kept. You can reinstall the app immediately.",false};
    case Operation::removeAll: {
        bool hadDriver=sun::driverPhase(driverFacts())!=sun::DriverPhase::absent;
        stopApp();if(hadDriver)removeDriver(progress);uninstall(progress);
        return {hadDriver,hadDriver?L"The app and USBPcap were removed. Saved profiles were kept. Restart Windows when convenient to finish removing the driver.":L"The app was removed. USBPcap was already absent. Your saved profiles were kept.",false};
    }
    case Operation::driverInstall:
        stopApp();installDriver(progress);return {true,L"USBPcap is installed. Restart Windows to activate it. Your normal sign-in startup setting is unchanged.",false};
    case Operation::driverReinstall:
        if(!std::filesystem::exists(installFolder()/L"Uninstall.exe"))throw std::runtime_error("Install the app first so driver setup can resume after restart.");
        stopApp();removeDriver(progress);resumeAfterSignIn(true);
        return {true,L"Step 1 of 2 is complete: USBPcap was removed. Restart Windows when ready. Driver setup will reopen after sign-in; choose Install driver there, then restart once more. No command prompt is needed.",false};
    }
    throw std::runtime_error("Unknown setup operation");
}
struct Work {
    Operation operation;std::thread thread;std::atomic_bool done{};Result result;std::string error;
};
HRESULT CALLBACK progressCallback(HWND hwnd,UINT message,WPARAM,LPARAM,LONG_PTR data) {
    auto work=reinterpret_cast<Work*>(data);
    if(message==TDN_CREATED) {
        SendMessageW(hwnd,TDM_SET_MARQUEE_PROGRESS_BAR,TRUE,0);SendMessageW(hwnd,TDM_SET_PROGRESS_BAR_MARQUEE,TRUE,30);
        SendMessageW(hwnd,TDM_ENABLE_BUTTON,IDCLOSE,FALSE);
        work->thread=std::thread([hwnd,work] {
            CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
            try {work->result=perform(work->operation,[hwnd](const std::wstring& text){SendMessageW(hwnd,TDM_SET_ELEMENT_TEXT,TDE_CONTENT,reinterpret_cast<LPARAM>(text.c_str()));});}
            catch(const std::exception& e){work->error=e.what();}
            CoUninitialize();work->done=true;
            SendMessageW(hwnd,TDM_ENABLE_BUTTON,IDCLOSE,TRUE);
            SendMessageW(hwnd,TDM_CLICK_BUTTON,IDCLOSE,0);
        });
    }
    if(message==TDN_BUTTON_CLICKED && !work->done)return S_FALSE;
    return S_OK;
}
Result withProgress(Operation operation) {
    Work work{operation};TASKDIALOGCONFIG dialog{sizeof(dialog)};
    dialog.pszWindowTitle=L"Sun Keyboard Tools Setup";dialog.pszMainInstruction=L"Applying your changes";
    dialog.pszContent=L"Preparing...";dialog.dwFlags=TDF_SHOW_MARQUEE_PROGRESS_BAR|TDF_SIZE_TO_CONTENT;
    dialog.dwCommonButtons=TDCBF_CLOSE_BUTTON;dialog.pfCallback=progressCallback;dialog.lpCallbackData=reinterpret_cast<LONG_PTR>(&work);
    auto hr=TaskDialogIndirect(&dialog,nullptr,nullptr,nullptr);
    if(work.thread.joinable())work.thread.join();
    if(FAILED(hr))throw std::runtime_error("Cannot show setup progress");
    if(!work.error.empty())throw std::runtime_error(work.error);return work.result;
}
int elevate(const wchar_t* args) {
    auto exe=sun::executable();SHELLEXECUTEINFOW info{sizeof(info)};
    info.fMask=SEE_MASK_NOCLOSEPROCESS;info.lpVerb=L"runas";info.lpFile=exe.c_str();info.lpParameters=args;info.nShow=SW_SHOWNORMAL;
    check(ShellExecuteExW(&info)!=FALSE,"Administrator permission");sun::Handle process(info.hProcess);
    WaitForSingleObject(process.value,INFINITE);DWORD code{};GetExitCodeProcess(process.value,&code);return static_cast<int>(code);
}
void finish(const Result& result) {
    TASKDIALOGCONFIG dialog{sizeof(dialog)};dialog.pszWindowTitle=L"Sun Keyboard Tools Setup";
    dialog.pszMainInstruction=result.reboot?L"Changes applied. A restart is needed.":L"Changes applied.";
    dialog.pszContent=result.message.c_str();dialog.dwFlags=TDF_SIZE_TO_CONTENT;dialog.dwCommonButtons=TDCBF_CLOSE_BUTTON;
    TASKDIALOG_BUTTON open{200,L"Open Sun Keyboard Tools"};if(result.launch){dialog.cButtons=1;dialog.pButtons=&open;dialog.nDefaultButton=200;}
    int clicked{};TaskDialogIndirect(&dialog,&clicked,nullptr,nullptr);
    if(clicked==200)ShellExecuteW(nullptr,L"open",(installFolder()/L"sun-keyboard-tools.exe").c_str(),nullptr,nullptr,SW_SHOWNORMAL);
}
int driverScreen() {
    auto facts=driverFacts();auto status=driverDescription(facts);
    TASKDIALOGCONFIG dialog{sizeof(dialog)};dialog.pszWindowTitle=L"Sun Keyboard Tools - Driver options";
    dialog.pszMainInstruction=L"Manage the USBPcap input driver";dialog.pszContent=status.c_str();
    dialog.pszExpandedInformation=L"Reinstall removes USBPcap before installing a fresh copy. Expect two Windows restarts. Setup reopens after the first sign-in to offer the installation step. Wireshark and other tools may also use this shared driver.\n\nThe included USBPcap 1.5.4.0 package is signed. An unresolved upstream Windows 11 crash report exists (github.com/desowin/usbpcap/issues/146). Setup never disables driver security or restarts Windows for you.";
    dialog.dwFlags=TDF_USE_COMMAND_LINKS|TDF_SIZE_TO_CONTENT;dialog.dwCommonButtons=TDCBF_CANCEL_BUTTON;
    bool absent=sun::mayInstallDriver(facts);
    TASKDIALOG_BUTTON buttons[]{{210,absent?L"Install driver\nInstall the signed USBPcap package bundled with this app.":L"Reinstall driver\nRemove USBPcap, then continue setup after restarting Windows."},{211,L"Open Windows Installed apps\nUse this for a custom or incomplete USBPcap installation."}};
    dialog.cButtons=2;dialog.pButtons=buttons;int clicked{};TaskDialogIndirect(&dialog,&clicked,nullptr,nullptr);
    if(clicked==211){ShellExecuteW(nullptr,L"open",L"ms-settings:appsfeatures",nullptr,nullptr,SW_SHOWNORMAL);return 0;}
    if(clicked!=210)return 0;
    if(!absent && sun::driverPhase(facts)==sun::DriverPhase::restart) {
        MessageBoxW(nullptr,L"Restart Windows before the next driver step, then reopen Sun Keyboard Tools Setup from Start.",L"Restart needed",MB_OK|MB_ICONINFORMATION);return 3010;
    }
    if(!absent && MessageBoxW(nullptr,L"Remove the shared USBPcap driver and prepare its reinstall?\n\nExtra-key remapping and other USBPcap users will stop until setup is finished. You will choose when to restart Windows.",L"Reinstall USBPcap",MB_YESNO|MB_ICONQUESTION)!=IDYES)return 0;
    return elevate(absent?L"--driver-install":L"--driver-reinstall");
}
int front(bool remove) {
    bool installed=std::filesystem::exists(installFolder()/L"sun-keyboard-tools.exe");
    auto title=L"Sun Keyboard Tools "+std::wstring(sun::wideVersion);
    TASKDIALOGCONFIG dialog{sizeof(dialog)};dialog.pszWindowTitle=title.c_str();
    dialog.pszMainInstruction=remove?L"Uninstall Sun Keyboard Tools?":installed?L"Keep your Sun keys working.":L"Make your Sun keyboard's extra keys useful again.";
    dialog.pszContent=remove?L"Removes the app and this account's automatic startup task. Your saved mappings are kept. The running remapper will be closed safely.":
        installed?L"Update or repair the app, manage the input driver, or remove the installation. Saved mappings are kept.":
        L"Installs the remapper, Start menu shortcuts, automatic startup, and the signed USBPcap input driver if needed. Administrator access is required. A driver installation normally needs a Windows restart.";
    dialog.pszExpandedInformation=L"Install location: Program Files\\Sun Keyboard Tools\n\nOriginal application code: no license selected. USBPcap retains its GPL/BSD licenses; its complete matching source is included.\n\nWindows 11 compatibility varies: USBPcap 1.5.4.0 has an unresolved upstream crash report (#146). Setup never changes Windows driver security or restarts the computer automatically.";
    dialog.dwFlags=TDF_USE_COMMAND_LINKS|TDF_SIZE_TO_CONTENT;dialog.dwCommonButtons=TDCBF_CANCEL_BUTTON;
    std::vector<TASKDIALOG_BUTTON> buttons;
    if(remove){buttons.push_back({220,L"Uninstall app"});dialog.pszVerificationText=L"Also remove USBPcap (shared with Wireshark and other tools)";}
    else {
        buttons.push_back({221,installed?L"Update or repair app\nRestore application files. Keep mappings, driver, and startup preference.":L"Install\nSet up the app, driver, and automatic startup."});
        if(installed){buttons.push_back({222,L"Driver options\nCheck the driver or reinstall it with guided restarts."});buttons.push_back({223,L"Uninstall\nRemove the app, with an optional driver removal."});}
    }
    dialog.cButtons=static_cast<UINT>(buttons.size());dialog.pButtons=buttons.data();int clicked{};BOOL alsoDriver{};
    check(SUCCEEDED(TaskDialogIndirect(&dialog,&clicked,nullptr,&alsoDriver)),"Show setup window");
    switch(clicked) {
    case 220:return elevate(alsoDriver?L"--remove-all":L"--remove");
    case 221:return elevate(L"--install");
    case 222:return driverScreen();
    case 223:return front(true);
    default:return 0;
    }
}
}
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int) {
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32|LOAD_LIBRARY_SEARCH_APPLICATION_DIR);
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);bool quiet=false;
    try {
        int argc{};auto raw=CommandLineToArgvW(GetCommandLineW(),&argc);std::vector<std::wstring> args(raw,raw+argc);LocalFree(raw);
        if(argc==3 && args[1]==L"--extract") {
            quiet=true;
            auto root=std::filesystem::absolute(args[2]);if(std::filesystem::exists(root))throw std::runtime_error("Extraction destination must not already exist");
            extract(root);trust(root/L"third-party"/L"USBPcapSetup-1.5.4.0.exe");return 0;
        }
        INITCOMMONCONTROLSEX cc{sizeof(cc),ICC_STANDARD_CLASSES};InitCommonControlsEx(&cc);
        std::wstring command=argc>1?args[1]:L"";
        quiet=argc>2 && args[2]==L"--quiet";
        Operation operation{};bool direct=true;
        if(command==L"--install")operation=Operation::install;
        else if(command==L"--remove")operation=Operation::remove;
        else if(command==L"--remove-all")operation=Operation::removeAll;
        else if(command==L"--driver-install")operation=Operation::driverInstall;
        else if(command==L"--driver-reinstall")operation=Operation::driverReinstall;
        else direct=false;
        if(direct) {
            if(!sun::elevated())throw std::runtime_error("Use the graphical setup window to request administrator access.");
            sun::Handle mutex(CreateMutexW(nullptr,FALSE,L"Global\\SunKeyboardTools.Maintenance"));
            if(!mutex)throw std::runtime_error("Cannot lock setup");
            auto lock=WaitForSingleObject(mutex.value,0);
            if(lock!=WAIT_OBJECT_0 && lock!=WAIT_ABANDONED)throw std::runtime_error("Another setup operation is already running.");
            Result result=quiet?perform(operation,[](const std::wstring&){}):withProgress(operation);
            if(!quiet)finish(result);return result.reboot?3010:0;
        }
        if(command==L"--driver")return driverScreen();
        return front(command==L"--uninstall");
    } catch(const std::exception& e) {
        if(quiet){std::ofstream out(sun::executable().parent_path()/L"setup-error.txt");out<<e.what();}
        else MessageBoxW(nullptr,sun::wide(e.what()).c_str(),L"Sun Keyboard Tools Setup",MB_OK|MB_ICONERROR);
        return 1;
    }
}

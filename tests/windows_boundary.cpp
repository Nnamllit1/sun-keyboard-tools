// Opt-in integration test. Requires the installed helper, a connected Type 7,
// and the UI running normally. Pauses/resumes capture without editing mappings.
#include "ipc.hpp"
#include <aclapi.h>
#include <sddl.h>
#include <fstream>
#include <iostream>

namespace {
void require(bool good,const char* why){if(!good)throw std::runtime_error(why);}
sun::Handle connect() {
    for(int i=0;i<20;++i){auto pipe=sun::connectCapture();if(pipe)return pipe;Sleep(100);}
    throw std::runtime_error("No installed helper connection");
}
}
int wmain(int argc,wchar_t** argv) {
    if(argc<2 || argc>3)return 2;
    bool idle=argc==3 && std::wstring_view(argv[2])==L"--idle";
    if(sun::elevated()){sun::launchNormal(sun::executable(),L"\""+std::wstring(argv[1])+L"\""+(idle?L" --idle":L""));return 0;}
    std::ofstream log(argv[1]);log<<std::unitbuf;HWND ui{};bool resume{};
    try {
        sun::hardenProcess();log<<"Tester: normal user\n";
        ui=FindWindowW(L"SunKeyboardTools.Settings",nullptr);require(ui!=nullptr,"UI is not running");
        DWORD pid{};GetWindowThreadProcessId(ui,&pid);sun::Handle app(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid));
        require(app && !sun::processElevated(app.value),"UI is elevated");log<<"UI: normal user\n";
        auto helper=sun::installFolder()/L"sun-keyboard-capture.exe";
        sun::Handle writable(CreateFileW(helper.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));
        require(!writable && GetLastError()==ERROR_ACCESS_DENIED,"Normal user can write helper image");log<<"Helper image write: denied\n";
        std::array<wchar_t,80> text{};GetWindowTextW(GetDlgItem(ui,102),text.data(),static_cast<int>(text.size()));
        if(std::wstring_view(text.data())==L"Pause remapping"){SendMessageW(ui,WM_COMMAND,102,0);resume=true;}
        sun::Handle peer;
        {
            auto pipe=connect();peer=sun::authenticatePeer(pipe.value,true);
            require(sun::processElevated(peer.value),"Capture helper is not elevated");log<<"Helper: elevated, correct protected image and sign-in\n";
            sun::Handle injection(OpenProcess(PROCESS_CREATE_THREAD|PROCESS_VM_WRITE|PROCESS_DUP_HANDLE,FALSE,GetProcessId(peer.value)));
            require(!injection,"Normal user can inject into or duplicate handles from helper");log<<"Helper process injection/handle access: denied\n";
            // An established legitimate server must not accept another unprivileged instance.
            sun::Handle extra(CreateNamedPipeW(sun::pipeName().c_str(),PIPE_ACCESS_DUPLEX,PIPE_TYPE_MESSAGE|PIPE_REJECT_REMOTE_CLIENTS,2,4096,4096,0,nullptr));
            require(!extra,"Normal user created another capture server instance");log<<"Additional pipe server: denied\n";
        }
        for(int scenario=0;scenario<4;++scenario) {
            auto pipe=connect();sun::wire::Request request;request.vid=0x430;request.pid=0xa2;request.endpoint=0x81;
            request.reportSize=8;request.keysOffset=2;request.keysCount=6;request.count=1;request.usages[0]=0x7c;
            std::array<BYTE,sizeof(request)+1> bytes{};DWORD size=sizeof(request);
            if(scenario==0)size=1;
            if(scenario==1)size=sizeof(bytes);
            if(scenario==2)request.keysOffset=UINT32_MAX;
            if(scenario==3)request.operation=static_cast<uint32_t>(sun::wire::Operation::shutdown);
            memcpy(bytes.data(),&request,sizeof(request));require(sun::pipeTransfer(pipe.value,bytes.data(),size,true),"Cannot send negative test");
            sun::wire::Frame frame;require(!sun::pipeTransfer(pipe.value,&frame,sizeof(frame),false,{},6000),"Unsafe request accepted");
        }
        log<<"Short, oversized, overflowing, and normal-user shutdown requests: rejected\n";
        {
            auto pipe=connect();auto profile=sun::readProfile(sun::installFolder()/L"profiles/sun-type7.ini");auto request=sun::wire::request(profile);
            require(sun::pipeTransfer(pipe.value,&request,sizeof(request),true),"Valid configuration failed");
            sun::wire::Frame frame;require(sun::pipeTransfer(pipe.value,&frame,sizeof(frame),false,{},6000) && sun::wire::valid(frame),"No valid capture response");
            require(frame.event==static_cast<uint32_t>(sun::wire::Event::state) && frame.value==static_cast<uint32_t>(sun::wire::State::ready),"Keyboard capture did not become ready");
        }
        log<<"Valid Type 7 request after attacks: ready\n";
        // A local fake server must fail peer validation, even with the correct account.
        auto name=L"\\\\.\\pipe\\SunKeyboardTools.Test."+std::to_wstring(GetCurrentProcessId());
        sun::Handle fake(CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED,PIPE_TYPE_MESSAGE|PIPE_REJECT_REMOTE_CLIENTS,1,4096,4096,0,nullptr));
        sun::Handle client(CreateFileW(name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION,nullptr));
        require(fake && client,"Cannot create isolated fake server test");bool rejected=false;
        try{sun::authenticatePeer(client.value,true);}catch(const std::exception&){rejected=true;}
        require(rejected,"Fake server authenticated");log<<"Normal-user fake server: rejected\n";
        if(idle) {
            require(resume,"Idle/resume check requires remapping initially active");
            require(WaitForSingleObject(peer.value,35000)==WAIT_OBJECT_0,"Unused helper did not exit");
            log<<"Idle helper exited\n";
        }
        if(resume){SendMessageW(ui,WM_COMMAND,102,0);resume=false;}
        if(idle) {
            // The UI owns the streaming connection again; verify its ready text.
            bool ready=false;
            for(int i=0;i<100 && !ready;++i) {
                EnumChildWindows(ui,[](HWND child,LPARAM context)->BOOL {
                    wchar_t value[512]{};GetWindowTextW(child,value,512);
                    if(std::wstring_view(value).starts_with(L"Ready. Extra keys"))*reinterpret_cast<bool*>(context)=true;return TRUE;
                },reinterpret_cast<LPARAM>(&ready));if(!ready)Sleep(100);
            }
            require(ready,"Resume did not restart capture through the startup task");log<<"Resume after helper exit: ready\n";
        }
        log<<"PASS: boundary checks; original remapping mode restored\n";return 0;
    }catch(const std::exception& e){log<<"FAIL: "<<e.what()<<'\n';if(resume && IsWindow(ui))SendMessageW(ui,WM_COMMAND,102,0);return 1;}
}

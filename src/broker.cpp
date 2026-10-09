#include "capture.hpp"
#include "ipc.hpp"
#include <shellapi.h>
#include <thread>

int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int) {
    try {
        sun::hardenProcess();
        if(!sun::elevated())return 1;
        auto root=sun::installFolder();auto own=sun::executable();
        if(_wcsicmp(own.c_str(),(root/L"sun-keyboard-capture.exe").c_str())!=0)return 1;
        auto protectedFiles=sun::lockProtected(own);
        auto protectedUi=sun::lockProtected(root/L"sun-keyboard-tools.exe");
        int argc{};auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);
        bool background=argc==2 && std::wstring_view(argv[1])==L"--background";
        bool manual=argc==2 && std::wstring_view(argv[1])==L"--capture";LocalFree(argv);
        if(!background && !manual)return 1;
        auto pipe=sun::createCapturePipe();
        if(background)sun::launchNormal(root/L"sun-keyboard-tools.exe",L"--background");
        sun::dropCapturePrivileges();
        for(;;) {
            sun::Handle event(CreateEventW(nullptr,TRUE,FALSE,nullptr));if(!event)return 1;
            OVERLAPPED ov{};ov.hEvent=event.value;
            if(!ConnectNamedPipe(pipe.value,&ov)) {
                auto code=GetLastError();
                if(code==ERROR_IO_PENDING) {
                    // No UI means no reason to retain administrator privileges.
                    if(WaitForSingleObject(event.value,30000)!=WAIT_OBJECT_0) {
                        CancelIoEx(pipe.value,&ov);DWORD done{};GetOverlappedResult(pipe.value,&ov,&done,TRUE);return 0;
                    }
                    DWORD done{};if(!GetOverlappedResult(pipe.value,&ov,&done,FALSE))return 1;
                } else if(code!=ERROR_PIPE_CONNECTED)return 1;
            }
            try {
                auto peer=sun::authenticatePeer(pipe.value,false);sun::wire::Request request;
                if(sun::pipeTransfer(pipe.value,&request,sizeof(request),false)) {
                    if(request.signature==sun::wire::magic && request.revision==sun::wire::version &&
                       request.operation==static_cast<uint32_t>(sun::wire::Operation::shutdown)) {
                        if(sun::processElevated(peer.value))return 0;
                    } else {
                        auto profile=sun::wire::profile(request);
                        std::stop_source session;
                        std::jthread worker([&] {
                            sun::captureKeyboard(session.get_token(),profile,[&](sun::wire::Frame frame){
                                if(!sun::pipeTransfer(pipe.value,&frame,sizeof(frame),true,session.get_token()))session.request_stop();
                            });
                        });
                        // No more commands on an established stream. Closing or sending
                        // anything cancels it. The elevated side never executes shortcuts.
                        sun::wire::Request unexpected;
                        sun::pipeTransfer(pipe.value,&unexpected,sizeof(unexpected),false,session.get_token(),INFINITE);
                        session.request_stop();worker.join();
                    }
                }
            } catch(const std::exception&) { /* Malformed/untrusted clients lose this connection. */ }
            DisconnectNamedPipe(pipe.value);
        }
    } catch(const std::exception&) {return 1;}
}

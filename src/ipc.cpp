#include "ipc.hpp"
#include <sddl.h>
#include <stdexcept>

namespace sun {
std::wstring pipeName(){return L"\\\\.\\pipe\\SunKeyboardTools.Capture.v1."+logonSid();}
Handle createCapturePipe() {
    // Generic write would also grant FILE_CREATE_PIPE_INSTANCE. Grant only the
    // data/attribute rights needed by the client, and reject remote connections.
    auto descriptor=L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;0x0012019b;;;"+logonSid()+L")S:(ML;;NW;;;ME)";
    PSECURITY_DESCRIPTOR sd{};
    if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(descriptor.c_str(),SDDL_REVISION_1,&sd,nullptr))throw std::runtime_error("Cannot secure capture pipe");
    SECURITY_ATTRIBUTES sa{sizeof(sa),sd,FALSE};
    Handle pipe(CreateNamedPipeW(pipeName().c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_MESSAGE|PIPE_READMODE_MESSAGE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,4096,4096,1000,&sa));
    LocalFree(sd);if(!pipe)throw std::runtime_error("Capture pipe already exists or cannot be created");return pipe;
}
bool pipeTransfer(HANDLE pipe,void* data,DWORD bytes,bool write,std::stop_token stop,DWORD timeout) {
    if(stop.stop_requested())return false;
    Handle event(CreateEventW(nullptr,TRUE,FALSE,nullptr));if(!event)return false;
    OVERLAPPED ov{};ov.hEvent=event.value;DWORD done{};
    auto ok=write?WriteFile(pipe,data,bytes,&done,&ov):ReadFile(pipe,data,bytes,&done,&ov);
    if(!ok && GetLastError()!=ERROR_IO_PENDING)return false;
    auto started=GetTickCount64();
    while(!ok) {
        auto wait=WaitForSingleObject(event.value,100);
        if(wait==WAIT_OBJECT_0){ok=GetOverlappedResult(pipe,&ov,&done,FALSE);break;}
        if(stop.stop_requested() || wait==WAIT_FAILED || GetTickCount64()-started>=timeout) {
            CancelIoEx(pipe,&ov);GetOverlappedResult(pipe,&ov,&done,TRUE);return false;
        }
    }
    return ok && done==bytes && !stop.stop_requested();
}
Handle authenticatePeer(HANDLE pipe,bool server) {
    ULONG id{};auto ok=server?GetNamedPipeServerProcessId(pipe,&id):GetNamedPipeClientProcessId(pipe,&id);
    if(!ok)throw std::runtime_error("Cannot identify capture peer");
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,id));
    if(!process || accountSid(process.value)!=accountSid() || logonSid(process.value)!=logonSid())throw std::runtime_error("Capture peer belongs to a different sign-in");
    DWORD session{},ownSession{};
    if(!ProcessIdToSessionId(id,&session) || !ProcessIdToSessionId(GetCurrentProcessId(),&ownSession) || session!=ownSession)throw std::runtime_error("Capture peer belongs to a different session");
    if(server) {
        std::wstring image(32768,L'\0');DWORD size=static_cast<DWORD>(image.size());
        if(!processElevated(process.value) || !QueryFullProcessImageNameW(process.value,0,image.data(),&size))throw std::runtime_error("Untrusted capture server");
        image.resize(size);auto expected=installFolder()/L"sun-keyboard-capture.exe";
        if(_wcsicmp(image.c_str(),expected.c_str())!=0)throw std::runtime_error("Unexpected capture server image");
        auto locks=lockProtected(expected);
    }
    return process; // Hold identity throughout the connection; never impersonate it.
}
Handle connectCapture() {
    WaitNamedPipeW(pipeName().c_str(),500);
    // Identification prevents a pipe squatter from impersonating this client.
    Handle pipe(CreateFileW(pipeName().c_str(),FILE_READ_DATA|FILE_WRITE_DATA|FILE_READ_ATTRIBUTES|FILE_WRITE_ATTRIBUTES|SYNCHRONIZE,
        0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED|SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION,nullptr));
    if(!pipe)return {};
    auto peer=authenticatePeer(pipe.value,true);DWORD mode=PIPE_READMODE_MESSAGE;
    if(!SetNamedPipeHandleState(pipe.value,&mode,nullptr,nullptr))throw std::runtime_error("Cannot configure capture connection");
    return pipe;
}
void stopCapture() {
    Handle pipe;
    for(int i=0;i<20 && !pipe;++i){pipe=connectCapture();if(!pipe && GetLastError()!=ERROR_PIPE_BUSY)return;if(!pipe)Sleep(100);}
    if(!pipe)throw std::runtime_error("Capture helper is busy");
    auto peer=authenticatePeer(pipe.value,true);wire::Request request;request.operation=static_cast<uint32_t>(wire::Operation::shutdown);
    if(!pipeTransfer(pipe.value,&request,sizeof(request),true))throw std::runtime_error("Cannot stop capture helper");
    pipe.reset();if(WaitForSingleObject(peer.value,15000)!=WAIT_OBJECT_0)throw std::runtime_error("Capture helper did not stop");
}
}

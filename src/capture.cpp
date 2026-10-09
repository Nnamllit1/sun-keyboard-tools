#include "capture.hpp"
#include "windows.hpp"
#include <winioctl.h>
#include <usbioctl.h>
#include <algorithm>
#include <chrono>
#include <thread>
#include <stdexcept>

namespace sun {
namespace {
constexpr DWORD setupBuffer=CTL_CODE(FILE_DEVICE_UNKNOWN,0x800,METHOD_BUFFERED,FILE_READ_ACCESS);
constexpr DWORD startFilter=CTL_CODE(FILE_DEVICE_UNKNOWN,0x801,METHOD_BUFFERED,FILE_READ_ACCESS|FILE_WRITE_ACCESS);
constexpr DWORD getHub=CTL_CODE(FILE_DEVICE_UNKNOWN,0x803,METHOD_BUFFERED,FILE_ANY_ACCESS);
constexpr DWORD setSnaplen=CTL_CODE(FILE_DEVICE_UNKNOWN,0x804,METHOD_BUFFERED,FILE_READ_ACCESS);
struct Target { std::wstring capture, hub; ULONG port{}; USHORT address{}; };
struct ConnectionInfo { USB_DEVICE_DESCRIPTOR DeviceDescriptor{}; bool DeviceIsHub{}; USHORT DeviceAddress{}; };
std::wstring hubPath(std::wstring path) {
    if(path.starts_with(L"\\??\\"))path=path.substr(4);
    if(path.starts_with(L"\\\\.\\") || path.starts_with(L"\\\\?\\"))path=path.substr(4);
    if(!path.starts_with(L"USB#") && !path.starts_with(L"usb#"))throw std::runtime_error("Unexpected USB hub interface");
    if(path.find_first_of(L"\\/:")!=std::wstring::npos || path.size()>3000)throw std::runtime_error("Invalid USB hub interface");
    return L"\\\\.\\GLOBALROOT\\GLOBAL??\\"+path;
}
Handle openHub(const std::wstring& path) {
    return Handle(CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr));
}
bool connection(HANDLE hub, ULONG port, ConnectionInfo& result) {
    alignas(USB_NODE_CONNECTION_INFORMATION_EX) std::array<uint8_t,4096> storage{};
    auto info=reinterpret_cast<USB_NODE_CONNECTION_INFORMATION_EX*>(storage.data());
    info->ConnectionIndex=port;
    DWORD bytes{};
    if(!DeviceIoControl(hub,IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX,info,sizeof(*info),
        info,static_cast<DWORD>(storage.size()),&bytes,nullptr) || bytes<sizeof(*info) || info->ConnectionStatus!=DeviceConnected)return false;
    result={info->DeviceDescriptor,info->DeviceIsHub!=FALSE,info->DeviceAddress};return true;
}
bool sameDevice(const ConnectionInfo& info,const Profile& p) {
    return !info.DeviceIsHub && info.DeviceDescriptor.idVendor==p.vid && info.DeviceDescriptor.idProduct==p.pid &&
        info.DeviceAddress>0 && info.DeviceAddress<128;
}
void walkHub(const std::wstring& path,const std::wstring& capture,const Profile& p,
             std::vector<Target>& targets,unsigned depth=0) {
    if (depth>8) return;
    auto hub=openHub(path);
    if (!hub) return;
    USB_NODE_INFORMATION node{}; node.NodeType=UsbHub;
    DWORD bytes{};
    if (!DeviceIoControl(hub.value,IOCTL_USB_GET_NODE_INFORMATION,&node,sizeof(node),&node,sizeof(node),&bytes,nullptr)) return;
    for (ULONG port=1;port<=node.u.HubInformation.HubDescriptor.bNumberOfPorts;++port) {
        ConnectionInfo info;
        if (!connection(hub.value,port,info)) continue;
        if (sameDevice(info,p)) targets.push_back({capture,path,port,info.DeviceAddress});
        if (!info.DeviceIsHub) continue;
        // A hub symbolic name is bounded independently of the returned size.
        alignas(USB_NODE_CONNECTION_NAME) std::array<uint8_t,8192> storage{};
        auto name=reinterpret_cast<USB_NODE_CONNECTION_NAME*>(storage.data());
        name->ConnectionIndex=port;
        if (!DeviceIoControl(hub.value,IOCTL_USB_GET_NODE_CONNECTION_NAME,name,sizeof(*name),
             name,static_cast<DWORD>(storage.size()),&bytes,nullptr)) continue;
        if (bytes<sizeof(*name) || bytes>storage.size() || name->ActualLength>bytes) continue;
        auto length=wcsnlen_s(name->NodeName,(bytes-FIELD_OFFSET(USB_NODE_CONNECTION_NAME,NodeName))/sizeof(wchar_t));
        if (!length || length>3000) continue;
        walkHub(hubPath(std::wstring(name->NodeName,length)),capture,p,targets,depth+1);
    }
}
std::vector<Target> findTargets(const Profile& p,bool& driverFound) {
    std::vector<Target> targets;
    std::vector<wchar_t> names(65536);
    if (!QueryDosDeviceW(nullptr,names.data(),static_cast<DWORD>(names.size())))
        throw std::runtime_error("Cannot enumerate Windows device names");
    for (auto name=names.data();*name;name+=wcslen(name)+1) {
        auto view=std::wstring_view(name);
        if (!view.starts_with(L"USBPcap") || view.size()<=7 || view.size()>10 ||
            !std::all_of(view.begin()+7,view.end(),[](wchar_t c){return c>=L'0' && c<=L'9';})) continue;
        driverFound=true;
        std::wstring capture=L"\\\\.\\GLOBALROOT\\Device\\"+std::wstring(name);
        Handle h(CreateFileW(capture.c_str(),0,0,nullptr,OPEN_EXISTING,0,nullptr));
        if (!h) continue;
        std::array<wchar_t,4096> path{}; DWORD bytes{};
        if (!DeviceIoControl(h.value,getHub,nullptr,0,path.data(),sizeof(path),&bytes,nullptr) ||
            bytes<sizeof(wchar_t) || bytes>sizeof(path)) continue;
        path.back()=0;
        walkHub(hubPath(path.data()),capture,p,targets);
    }
    return targets;
}
void control(HANDLE h,DWORD code,void* data,DWORD size) {
    Handle event(CreateEventW(nullptr,TRUE,FALSE,nullptr));
    if (!event) throw std::runtime_error("Cannot create capture event");
    OVERLAPPED ov{}; ov.hEvent=event.value; DWORD bytes{};
    if (!DeviceIoControl(h,code,data,size,nullptr,0,&bytes,&ov)) {
        if (GetLastError()!=ERROR_IO_PENDING) throw std::runtime_error("USBPcap setup failed: "+utf8(winError()));
        if (WaitForSingleObject(event.value,5000)!=WAIT_OBJECT_0) {
            CancelIoEx(h,&ov); GetOverlappedResult(h,&ov,&bytes,TRUE);
            throw std::runtime_error("USBPcap setup timed out");
        }
        if (!GetOverlappedResult(h,&ov,&bytes,FALSE)) throw std::runtime_error("USBPcap setup failed: "+utf8(winError()));
    }
}
}
void captureKeyboard(std::stop_token stop,const Profile& p,const std::function<void(wire::Frame)>& emit) {
    auto message=[&](wire::State state){emit({wire::magic,wire::version,static_cast<uint32_t>(wire::Event::state),static_cast<uint32_t>(state)});};
    while (!stop.stop_requested()) {
        try {
            bool driverFound=false;
            auto targets=findTargets(p,driverFound);
            if (targets.empty()) message(driverFound?wire::State::missingKeyboard:wire::State::missingDriver);
            else if (targets.size()!=1) message(wire::State::multipleKeyboards);
            else {
                auto target=targets.front();
                auto hub=openHub(target.hub);
                Handle capture(CreateFileW(target.capture.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr));
                if (!capture || !hub) throw std::runtime_error("Cannot open capture device: "+utf8(winError())+" Close other USB capture apps.");
                DWORD snap=512, bufferSize=65536;
                control(capture.value,setSnaplen,&snap,sizeof(snap));
                control(capture.value,setupBuffer,&bufferSize,sizeof(bufferSize));
                std::array<uint8_t,17> filter{};
                filter[target.address/8] |= static_cast<uint8_t>(1u << (target.address%8));
                control(capture.value,startFilter,filter.data(),static_cast<DWORD>(filter.size()));
                Handle event(CreateEventW(nullptr,TRUE,FALSE,nullptr));
                if (!event) throw std::runtime_error("Cannot create read event");
                std::array<uint8_t,16384> bytes{};
                PcapStream stream; Edges edges;
                message(wire::State::ready);
                bool valid=true;
                while (valid && !stop.stop_requested()) {
                    OVERLAPPED ov{}; ov.hEvent=event.value; ResetEvent(event.value);
                    DWORD read{};
                    bool pending=false;
                    if (!ReadFile(capture.value,bytes.data(),static_cast<DWORD>(bytes.size()),&read,&ov)) {
                        if (GetLastError()!=ERROR_IO_PENDING) throw std::runtime_error("Capture read failed: "+utf8(winError()));
                        pending=true;
                    }
                    while (pending) {
                        auto wait=WaitForSingleObject(event.value,200);
                        ConnectionInfo info;
                        valid=connection(hub.value,target.port,info) && sameDevice(info,p) && info.DeviceAddress==target.address;
                        if (stop.stop_requested() || !valid || wait==WAIT_FAILED) {
                            CancelIoEx(capture.value,&ov); GetOverlappedResult(capture.value,&ov,&read,TRUE);
                            read=0; break;
                        }
                        if (wait==WAIT_OBJECT_0) {
                            if (!GetOverlappedResult(capture.value,&ov,&read,FALSE)) throw std::runtime_error("Capture stopped: "+utf8(winError()));
                            break;
                        }
                    }
                    if (stop.stop_requested() || !valid) break;
                    ConnectionInfo info;
                    valid=connection(hub.value,target.port,info) && sameDevice(info,p) && info.DeviceAddress==target.address;
                    if (!valid) break;
                    if (!read) throw std::runtime_error("Capture stream ended");
                    stream.feed(std::span(bytes).first(read),[&](auto packet){
                        if (stop.stop_requested()) return;
                        auto report=keyboardReport(packet,p,target.address);
                        if (report.empty()) return;
                        emit({wire::magic,wire::version,static_cast<uint32_t>(wire::Event::report),0});
                        for(auto usage:edges.update(report,p))for(const auto& mapping:p.mappings)if(usage==mapping.usage)
                            emit({wire::magic,wire::version,static_cast<uint32_t>(wire::Event::key),usage});
                    });
                }
                message(wire::State::disconnected);
            }
        } catch (const std::exception&) {message(wire::State::captureError);}
        for (int i=0;i<15 && !stop.stop_requested();++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}
}

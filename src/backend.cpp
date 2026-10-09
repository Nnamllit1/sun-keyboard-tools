#include "backend.hpp"
#include "ipc.hpp"
#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace sun {
namespace {
bool inject(const Chord& chord) {
    if (!chord.key) return true;
    auto held=[](int vk){return (GetAsyncKeyState(vk)&0x8000)!=0;};
    for (int vk:{VK_CONTROL,VK_MENU,VK_SHIFT,VK_LWIN}) {
        bool down=held(vk) || (vk==VK_LWIN && held(VK_RWIN));
        if (down && std::find(chord.modifiers.begin(),chord.modifiers.end(),vk)==chord.modifiers.end()) return false;
    }
    if (held(chord.key)) return false;
    std::vector<INPUT> inputs;
    auto add=[&](WORD key,bool up) {
        INPUT in{}; in.type=INPUT_KEYBOARD; in.ki.wVk=key;
        in.ki.dwFlags=up?KEYEVENTF_KEYUP:0;
        if ((key>=VK_PRIOR && key<=VK_DOWN) || key==VK_INSERT || key==VK_DELETE || key==VK_LWIN)
            in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        inputs.push_back(in);
    };
    std::vector<uint16_t> added;
    for (auto vk:chord.modifiers) if (!held(vk) && !(vk==VK_LWIN && held(VK_RWIN))) {add(vk,false); added.push_back(vk);}
    add(chord.key,false); add(chord.key,true);
    for (auto it=added.rbegin();it!=added.rend();++it) add(*it,true);
    auto sent=SendInput(static_cast<UINT>(inputs.size()),inputs.data(),sizeof(INPUT));
    if (sent==inputs.size()) return true;
    // Release only keys actually pressed by the successful prefix of our batch.
    std::set<WORD> pending;
    for (size_t i=0;i<sent;++i) {
        if (inputs[i].ki.dwFlags&KEYEVENTF_KEYUP) pending.erase(inputs[i].ki.wVk);
        else pending.insert(inputs[i].ki.wVk);
    }
    inputs.clear();
    for (auto key:pending) add(key,true);
    if (!inputs.empty()) SendInput(static_cast<UINT>(inputs.size()),inputs.data(),sizeof(INPUT));
    return false;
}
}
Status Backend::status(){std::lock_guard lock(mutex_); return status_;}
void Backend::stop(){if (thread_.joinable()){thread_.request_stop(); thread_.join();}}
void Backend::start(Profile p,bool monitor) {
    stop();
    {std::lock_guard lock(mutex_); status_={};}
    thread_=std::jthread([this,p=std::move(p),monitor](std::stop_token stop){run(stop,p,monitor);});
}
void Backend::run(std::stop_token stop,Profile p,bool monitor) {
    auto message=[&](std::wstring text,bool connected=false){std::lock_guard lock(mutex_);status_.message=std::move(text);status_.connected=connected;};
    while(!stop.stop_requested()) {
        try {
            if(elevated())throw std::runtime_error("Shortcut injection requires a normal user process");
            auto pipe=connectCapture();
            if(!pipe)throw std::runtime_error("Capture helper is stopped. Click Pause, then Resume to start it.");
            auto peer=authenticatePeer(pipe.value,true);
            auto request=wire::request(p);
            if(!pipeTransfer(pipe.value,&request,sizeof(request),true,stop))throw std::runtime_error("Cannot configure capture helper");
            wire::Frame frame;
            while(!stop.stop_requested() && pipeTransfer(pipe.value,&frame,sizeof(frame),false,stop,INFINITE)) {
                if(!wire::valid(frame))throw std::runtime_error("Invalid capture event");
                if(frame.event==static_cast<uint32_t>(wire::Event::state)) {
                    switch(static_cast<wire::State>(frame.value)) {
                    case wire::State::ready:message(monitor?L"Key monitor active. Actions are disabled.":L"Ready. Extra keys now run your shortcuts.",true);break;
                    case wire::State::missingDriver:message(L"USBPcap is not active. Open Setup to check the driver.");break;
                    case wire::State::missingKeyboard:message(L"No matching keyboard available. Check the profile and connection.");break;
                    case wire::State::multipleKeyboards:message(L"More than one matching keyboard found. Connect one to continue.");break;
                    case wire::State::disconnected:message(L"Keyboard disconnected. Waiting for it to reconnect...");break;
                    case wire::State::captureError:message(L"Cannot capture keyboard input. Check USBPcap and close other USB capture apps.");break;
                    default:message(L"Starting capture...");break;
                    }
                } else if(frame.event==static_cast<uint32_t>(wire::Event::report)) {
                    std::lock_guard lock(mutex_);++status_.reports;
                } else for(const auto& m:p.mappings)if(m.usage==frame.value) {
                    auto chord=parseChord(m.shortcut);bool sent=!monitor && chord.key && inject(chord);
                    std::lock_guard lock(mutex_);++status_.presses;
                    if(sent)++status_.sent;else if(!monitor && chord.key)++status_.skipped;
                    status_.lastKey=wide(m.name+" -> "+m.shortcut)+(monitor?L" (monitor only)":!chord.key?L" (disabled)":sent?L"":L" (skipped: held key or blocked input)");
                }
            }
            message(L"Capture helper disconnected. Reconnecting...");
        } catch(const std::exception& e){message(wide(e.what()));}
        for(int i=0;i<10 && !stop.stop_requested();++i)std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}
}

#pragma once
#include "core.hpp"
#include <stdexcept>

namespace sun::wire {
constexpr uint32_t magic=0x534B5435,version=1;
enum class Operation:uint32_t {capture=1,shutdown=2};
enum class State:uint32_t {starting,ready,missingDriver,missingKeyboard,multipleKeyboards,disconnected,captureError};
enum class Event:uint32_t {state=1,report=2,key=3};
struct Request {
    uint32_t signature{magic},revision{version},operation{static_cast<uint32_t>(Operation::capture)};
    uint32_t vid{},pid{},endpoint{},reportSize{},keysOffset{},keysCount{},reportId{256},count{};
    std::array<uint32_t,32> usages{};
};
struct Frame {uint32_t signature{magic},revision{version},event{},value{};};
static_assert(sizeof(Request)==172 && sizeof(Frame)==16);
inline Profile profile(const Request& r) {
    if(r.signature!=magic || r.revision!=version || r.operation!=static_cast<uint32_t>(Operation::capture) ||
       !r.vid || r.vid>65535 || !r.pid || r.pid>65535 || r.endpoint<0x81 || r.endpoint>0x8f ||
       r.reportSize<1 || r.reportSize>256 || r.keysOffset>=r.reportSize || !r.keysCount ||
       r.keysCount>32 || r.keysCount>r.reportSize-r.keysOffset || r.reportId>256 || (r.reportId!=256 && r.keysOffset==0) ||
       !r.count || r.count>r.usages.size())throw std::runtime_error("Invalid capture request");
    Profile p;p.vid=static_cast<uint16_t>(r.vid);p.pid=static_cast<uint16_t>(r.pid);p.endpoint=static_cast<uint8_t>(r.endpoint);
    p.reportSize=r.reportSize;p.keysOffset=r.keysOffset;p.keysCount=r.keysCount;p.reportId=r.reportId==256?-1:static_cast<int>(r.reportId);
    std::set<uint32_t> seen;
    for(size_t i=0;i<r.usages.size();++i) {
        if(i>=r.count){if(r.usages[i])throw std::runtime_error("Nonzero capture request padding");continue;}
        if(r.usages[i]<4 || r.usages[i]>255 || !seen.insert(r.usages[i]).second)throw std::runtime_error("Invalid capture usage");
        p.mappings.push_back({static_cast<uint8_t>(r.usages[i]),{}, {}});
    }
    return p;
}
inline Request request(const Profile& p) {
    Request r;r.vid=p.vid;r.pid=p.pid;r.endpoint=p.endpoint;r.reportSize=p.reportSize;r.keysOffset=p.keysOffset;r.keysCount=p.keysCount;
    r.reportId=p.reportId<0?256:static_cast<uint32_t>(p.reportId);r.count=static_cast<uint32_t>(p.mappings.size());
    if(r.count>r.usages.size())throw std::runtime_error("Too many capture keys");
    for(size_t i=0;i<p.mappings.size();++i)r.usages[i]=p.mappings[i].usage;
    (void)profile(r);return r;
}
inline bool valid(const Frame& f) {
    if(f.signature!=magic || f.revision!=version)return false;
    switch(static_cast<Event>(f.event)) {
    case Event::state:return f.value<=static_cast<uint32_t>(State::captureError);
    case Event::report:return f.value==0;
    case Event::key:return f.value>=4 && f.value<=255;
    default:return false;
    }
}
}

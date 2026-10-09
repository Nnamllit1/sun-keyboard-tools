#include "core.hpp"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>

namespace sun {
namespace {
std::string trim(std::string s) {
    auto start = s.find_first_not_of(" \t\r\n");
    return start == s.npos ? "" : s.substr(start, s.find_last_not_of(" \t\r\n") - start + 1);
}
std::string upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
unsigned number(const std::string& s, unsigned max) {
    size_t used{};
    if (s.empty() || s[0] == '-') throw std::runtime_error("Expected a positive number: " + s);
    auto n = std::stoul(s, &used, 0);
    if (used != s.size() || n > max) throw std::runtime_error("Number out of range: " + s);
    return static_cast<unsigned>(n);
}
uint16_t u16(std::span<const uint8_t> b, size_t o) { return b[o] | (uint16_t(b[o+1]) << 8); }
uint32_t u32(std::span<const uint8_t> b, size_t o) { return u16(b,o) | (uint32_t(u16(b,o+2)) << 16); }
}
Chord parseChord(const std::string& text) {
    Chord result;
    if (upper(trim(text)) == "DISABLED") return result;
    std::istringstream in(text);
    std::string token;
    const std::map<std::string, int> mods{{"CTRL",0x11},{"ALT",0x12},{"SHIFT",0x10},{"WIN",0x5b}};
    const std::map<std::string, int> named{{"ESC",0x1b},{"TAB",9},{"ENTER",13},{"SPACE",32},
        {"BACKSPACE",8},{"DELETE",0x2e},{"INSERT",0x2d},{"HOME",0x24},{"END",0x23},
        {"PAGEUP",0x21},{"PAGEDOWN",0x22},{"LEFT",0x25},{"UP",0x26},{"RIGHT",0x27},{"DOWN",0x28}};
    while (std::getline(in, token, '+')) {
        token = upper(trim(token));
        if (result.key) throw std::runtime_error("Put modifiers first, followed by one key: " + text);
        if (auto m = mods.find(token); m != mods.end()) {
            if (std::find(result.modifiers.begin(),result.modifiers.end(),m->second)!=result.modifiers.end())
                throw std::runtime_error("Duplicate modifier: " + text);
            result.modifiers.push_back(static_cast<uint16_t>(m->second));
        } else if (auto k = named.find(token); k != named.end()) result.key = static_cast<uint16_t>(k->second);
        else if (token.size()==1 && ((token[0]>='A' && token[0]<='Z') || (token[0]>='0' && token[0]<='9')))
            result.key = static_cast<uint16_t>(token[0]);
        else if (token.size()>1 && token[0]=='F') {
            auto f = number(token.substr(1),24);
            if (!f) throw std::runtime_error("Function keys start at F1");
            result.key = static_cast<uint16_t>(0x70 + f - 1);
        } else throw std::runtime_error("Unknown shortcut key: " + token);
    }
    if (!result.key || text.empty() || text.back()=='+') throw std::runtime_error("Incomplete shortcut: " + text);
    return result;
}
Profile readProfile(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("Cannot open profile");
    Profile p;
    std::string line, section;
    std::set<std::string> fields;
    size_t lines{};
    std::array<char,1026> boundedLine{};
    while (in.getline(boundedLine.data(),static_cast<std::streamsize>(boundedLine.size()))) {
        line=boundedLine.data();
        if (++lines > 512 || line.size()>1024) throw std::runtime_error("Profile is too large");
        line = trim(line);
        if (line.empty() || line[0]=='#' || line[0]==';') continue;
        if (line.front()=='[' && line.back()==']') {
            section = line.substr(1,line.size()-2);
            if (section!="device" && section!="report" && section!="keys") throw std::runtime_error("Unknown profile section");
            continue;
        }
        auto eq = line.find('=');
        if (eq==line.npos) throw std::runtime_error("Expected name=value in profile");
        auto key = trim(line.substr(0,eq)), value = trim(line.substr(eq+1));
        if (!fields.insert(section+"/"+key).second) throw std::runtime_error("Duplicate profile field");
        if (section=="device") {
            if (key=="name") p.name=value;
            else if (key=="vid") p.vid=static_cast<uint16_t>(number(value,65535));
            else if (key=="pid") p.pid=static_cast<uint16_t>(number(value,65535));
            else throw std::runtime_error("Unknown device field: " + key);
        } else if (section=="report") {
            if (key=="endpoint") p.endpoint=static_cast<uint8_t>(number(value,255));
            else if (key=="size") p.reportSize=number(value,256);
            else if (key=="keys_offset") p.keysOffset=number(value,255);
            else if (key=="keys_count") p.keysCount=number(value,32);
            else if (key=="report_id") p.reportId=static_cast<int>(number(value,255));
            else throw std::runtime_error("Unknown report field: " + key);
        } else if (section=="keys") {
            auto usage=number(key,255);
            auto split=value.find('|');
            if (usage<4 || split==value.npos || p.mappings.size()>=32) throw std::runtime_error("Invalid key mapping");
            auto label=trim(value.substr(0,split)), shortcut=trim(value.substr(split+1));
            if (label.empty() || label.size()>32) throw std::runtime_error("Key label must have 1 to 32 characters");
            parseChord(shortcut);
            for (auto& m:p.mappings) if (m.usage==usage) throw std::runtime_error("Duplicate HID usage");
            p.mappings.push_back({static_cast<uint8_t>(usage),label,shortcut});
        } else throw std::runtime_error("Profile field outside a section");
    }
    if(!in.eof())throw std::runtime_error("Profile line is too large or unreadable");
    if (p.name.empty() || p.name.size()>80 || !p.vid || !p.pid || p.mappings.empty() ||
        !(p.endpoint & 0x80) || !(p.endpoint & 0x0f) || (p.endpoint & 0x70) ||
        !p.keysCount || p.keysOffset+p.keysCount>p.reportSize || (p.reportId>=0 && !p.keysOffset))
        throw std::runtime_error("Invalid device identity or report layout");
    return p;
}
void writeProfile(const std::filesystem::path& path, const Profile& p) {
    auto temp=path; temp += L".tmp";
    std::ofstream out(temp, std::ios::trunc);
    out << "# Sun Keyboard Tools profile; shortcuts use Windows virtual keys.\n[device]\nname=" << p.name
        << "\nvid=0x" << std::hex << p.vid << "\npid=0x" << p.pid << "\n\n[report]\nendpoint=0x"
        << unsigned(p.endpoint) << std::dec << "\nsize=" << p.reportSize << "\nkeys_offset=" << p.keysOffset
        << "\nkeys_count=" << p.keysCount << '\n';
    if (p.reportId>=0) out << "report_id=" << p.reportId << '\n';
    out << "\n[keys]\n";
    for (auto& m:p.mappings) out << "0x" << std::hex << unsigned(m.usage) << '=' << m.name << '|' << m.shortcut << '\n';
    out.close();
    if (!out) throw std::runtime_error("Could not save profile");
    // Replacement is performed by the Windows caller using MoveFileEx(REPLACE_EXISTING).
}
void PcapStream::feed(std::span<const uint8_t> bytes, const std::function<void(std::span<const uint8_t>)>& packet) {
    constexpr size_t limit=1024*1024;
    if (bytes.size()>limit || buffer_.size()+bytes.size()>limit) throw std::runtime_error("Capture buffer exceeds limit");
    buffer_.insert(buffer_.end(),bytes.begin(),bytes.end());
    size_t offset{};
    if (!header_) {
        if (buffer_.size()<24) return;
        if (u32(buffer_,0)!=0xa1b2c3d4 || u16(buffer_,4)!=2 || u16(buffer_,6)!=4 || u32(buffer_,20)!=249)
            throw std::runtime_error("Unsupported capture header");
        snaplen_=u32(buffer_,16);
        if (snaplen_<27 || snaplen_>65536) throw std::runtime_error("Invalid capture snapshot length");
        offset=24; header_=true;
    }
    while (buffer_.size()-offset>=16) {
        auto record=std::span<const uint8_t>(buffer_).subspan(offset);
        auto n=u32(record,8), original=u32(record,12);
        if (n>snaplen_ || n>original || n<27) throw std::runtime_error("Invalid capture record length");
        if (record.size()<16+n) break;
        if (n==original) packet(record.subspan(16,n)); // Ignore truncated USB transfers.
        offset += 16+n;
    }
    buffer_.erase(buffer_.begin(),buffer_.begin()+offset);
}
std::span<const uint8_t> keyboardReport(std::span<const uint8_t> b, const Profile& p, uint16_t address) {
    if (b.size()<27) return {};
    auto header=u16(b,0);
    if (header<27 || header>b.size() || u32(b,10)!=0 || u16(b,14)!=9 || !(b[16]&1) ||
        u16(b,19)!=address || b[21]!=p.endpoint || b[22]!=1 ||
        u32(b,23)!=p.reportSize || b.size()-header!=p.reportSize) return {};
    auto report=b.subspan(header);
    if (p.reportId>=0 && report[0]!=p.reportId) return {};
    return report;
}
std::vector<uint8_t> Edges::update(std::span<const uint8_t> report, const Profile& p) {
    if (report.size()!=p.reportSize || p.keysOffset+p.keysCount>report.size()) return {};
    std::set<uint8_t> keys;
    for (auto k:report.subspan(p.keysOffset,p.keysCount)) {
        if (k>=1 && k<=3) return {}; // Rollover/error is not a release.
        if (k) keys.insert(k);
    }
    std::vector<uint8_t> pressed;
    if (primed_) for (auto k:keys) if (!held_.contains(k)) pressed.push_back(k);
    held_=std::move(keys); primed_=true;
    return pressed;
}
}

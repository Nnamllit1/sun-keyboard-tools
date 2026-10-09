#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace sun {
struct Chord { std::vector<uint16_t> modifiers; uint16_t key{}; };
Chord parseChord(const std::string& text);
struct Mapping { uint8_t usage{}; std::string name, shortcut; };
struct Profile {
    std::string name;
    uint16_t vid{}, pid{};
    uint8_t endpoint{0x81};
    unsigned reportSize{8}, keysOffset{2}, keysCount{6};
    int reportId{-1};
    std::vector<Mapping> mappings;
};
Profile readProfile(const std::filesystem::path& path);
void writeProfile(const std::filesystem::path& path, const Profile& profile);

// USBPcap emits a byte stream: headers and records can cross ReadFile boundaries.
class PcapStream {
    std::vector<uint8_t> buffer_;
    bool header_{};
    uint32_t snaplen_{};
public:
    void feed(std::span<const uint8_t> bytes,
              const std::function<void(std::span<const uint8_t>)>& packet);
};
std::span<const uint8_t> keyboardReport(std::span<const uint8_t> packet,
                                       const Profile& profile, uint16_t address);
class Edges {
    std::set<uint8_t> held_;
    bool primed_{};
public:
    // First valid report establishes state, preventing a held key firing on reconnect.
    std::vector<uint8_t> update(std::span<const uint8_t> report, const Profile& profile);
};
}

#include "core.hpp"
#include <fstream>
#include <chrono>
#include <iostream>
#include <stdexcept>

namespace {
int checks{};
void require(bool condition,const char* text){++checks;if(!condition)throw std::runtime_error(text);}
template<class F> void rejects(F f){bool threw=false;try{f();}catch(const std::exception&){threw=true;}require(threw,"Malformed input was accepted");}
void put16(std::vector<uint8_t>& b,size_t o,uint16_t n){b[o]=uint8_t(n);b[o+1]=uint8_t(n>>8);}
void put32(std::vector<uint8_t>& b,size_t o,uint32_t n){put16(b,o,uint16_t(n));put16(b,o+2,uint16_t(n>>16));}
std::vector<uint8_t> packet(uint8_t key=0){
    std::vector<uint8_t>b(35);put16(b,0,27);put16(b,14,9);b[16]=1;put16(b,19,7);b[21]=0x81;b[22]=1;put32(b,23,8);b[29]=key;return b;
}
std::vector<uint8_t> capture(const std::vector<uint8_t>& p){
    std::vector<uint8_t>b(40);put32(b,0,0xa1b2c3d4);put16(b,4,2);put16(b,6,4);put32(b,16,512);put32(b,20,249);put32(b,32,uint32_t(p.size()));put32(b,36,uint32_t(p.size()));b.insert(b.end(),p.begin(),p.end());return b;
}
}
int main(int argc,char**argv){
    try{
        if(argc!=2)throw std::runtime_error("Pass the profiles directory");
        auto profile=sun::readProfile(std::filesystem::path(argv[1])/"sun-type7.ini");
        require(profile.vid==0x430 && profile.pid==0xa2 && profile.mappings.size()==11,"Type 7 identity/defaults");
        auto chord=sun::parseChord("Ctrl+Shift+Z");require(chord.key=='Z' && chord.modifiers.size()==2,"Chord parse");
        require(!sun::parseChord("Disabled").key,"Disabled");
        for(auto text:{"","Ctrl+","Ctrl+Ctrl+C","C+Ctrl","F0","F25","Ctrl+C+V","Win+Foo","Alt++A","Ctrl+F1junk"})rejects([&]{sun::parseChord(text);});
        auto p=packet(0x7c);auto report=sun::keyboardReport(p,profile,7);require(report.size()==8,"Valid report rejected");
        require(sun::keyboardReport(p,profile,8).empty(),"Wrong device accepted");
        for(auto [offset,value]:std::vector<std::pair<size_t,int>>{{0,26},{0,90},{10,1},{14,8},{16,0},{21,0x82},{22,3},{23,7}}){auto bad=p;bad[offset]=static_cast<uint8_t>(value);require(sun::keyboardReport(bad,profile,7).empty(),"Invalid USB header accepted");}
        auto shortPacket=p;shortPacket.pop_back();require(sun::keyboardReport(shortPacket,profile,7).empty(),"Truncated report");
        sun::Edges edges;
        require(edges.update(report,profile).empty(),"Held key fired at attach");
        auto release=packet();edges.update(sun::keyboardReport(release,profile,7),profile);
        require(edges.update(report,profile)==std::vector<uint8_t>{0x7c},"Copy press not detected");
        require(edges.update(report,profile).empty(),"Held copy repeated");
        auto rollover=packet(1);edges.update(sun::keyboardReport(rollover,profile,7),profile);
        require(edges.update(report,profile).empty(),"Rollover caused duplicate action");
        edges.update(sun::keyboardReport(release,profile,7),profile);
        require(edges.update(report,profile).size()==1,"Second press missing");
        auto dup=packet(0x7c);dup[30]=0x7c;sun::Edges two;two.update(sun::keyboardReport(release,profile,7),profile);
        require(two.update(sun::keyboardReport(dup,profile,7),profile).size()==1,"Duplicate HID usage fired twice");
        auto bytes=capture(p);
        for(size_t split=0;split<=bytes.size();++split){sun::PcapStream stream;int count=0;auto cb=[&](auto got){++count;require(std::vector<uint8_t>(got.begin(),got.end())==p,"Payload changed");};stream.feed(std::span(bytes).first(split),cb);stream.feed(std::span(bytes).subspan(split),cb);require(count==1,"Fragmented record lost/duplicated");}
        sun::PcapStream bytewise;int count=0;for(auto& byte:bytes)bytewise.feed(std::span(&byte,1),[&](auto){++count;});require(count==1,"Bytewise stream failed");
        auto multiple=bytes;multiple.insert(multiple.end(),bytes.begin()+24,bytes.end());sun::PcapStream combined;count=0;combined.feed(multiple,[&](auto){++count;});require(count==2,"Coalesced records lost");
        for(auto offset:{size_t(0),size_t(4),size_t(20),size_t(34)}){auto bad=bytes;bad[offset]=0xff;rejects([&]{sun::PcapStream s;s.feed(bad,[](auto){});});}
        auto truncated=bytes;put32(truncated,36,36);sun::PcapStream s;s.feed(truncated,[&](auto){throw std::runtime_error("Truncated packet dispatched");});
        auto temp=std::filesystem::current_path()/("sun-profile-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".ini");
        struct Cleanup{std::filesystem::path p;~Cleanup(){std::error_code e;std::filesystem::remove(p,e);p+=L".tmp";std::filesystem::remove(p,e);}} cleanup{temp};
        const std::string identity="[device]\nname=Test\nvid=0x430\npid=0xa2\n";
        const std::string keys="[keys]\n0x7c=Copy|Ctrl+C\n";
        for(auto text:{identity+"[report]\nsize=2\n"+keys,identity+"[report]\nendpoint=1\n"+keys,
            identity+"[report]\nkeys_count=0\n"+keys,identity+"[report]\nreport_id=256\n"+keys,
            identity+keys+"124=Again|Ctrl+Y\n",identity+keys+"0x7c=Copy|Ctrl+C\n",
            identity+"[keys]\n0x7c=Copy|Ctrl+NoSuchKey\n",identity+"[keys]\n1=Error|Ctrl+C\n",
            identity+"[report]\nunrecognized=5\n"+keys}){
            {std::ofstream out(temp);out<<text;}rejects([&]{sun::readProfile(temp);});
        }
        profile.mappings[5].shortcut="Ctrl+Shift+C";sun::writeProfile(temp,profile);
        auto saved=temp;saved+=L".tmp";auto loaded=sun::readProfile(saved);
        require(loaded.mappings[5].shortcut=="Ctrl+Shift+C" && loaded.vid==profile.vid,"Saved profile did not round-trip");
        std::cout<<checks<<" checks passed\n";return 0;
    }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
}

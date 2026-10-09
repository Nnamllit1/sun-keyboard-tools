#include "backend.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>

// Bounded, non-injecting hardware check. Never logs ordinary keys or raw reports.
int wmain(int argc,wchar_t** argv){
    try{
        if(argc!=4){std::cerr<<"Usage: sun-key-monitor PROFILE.ini SECONDS OUTPUT.txt\n";return 2;}
        int seconds=std::stoi(argv[2]);if(seconds<1||seconds>300)throw std::runtime_error("Duration must be 1 to 300 seconds");
        if(sun::elevated())throw std::runtime_error("Run the monitor without administrator privileges");
        auto profile=sun::readProfile(argv[1]);
        std::ofstream output(argv[3]);if(!output)throw std::runtime_error("Cannot open output file");
        sun::Backend backend;backend.start(profile,true);
        uint64_t lastPress=0;std::wstring previous;
        for(int i=0;i<seconds*5;++i){
            std::this_thread::sleep_for(std::chrono::milliseconds(200));auto status=backend.status();
            if(status.message!=previous){output<<sun::utf8(status.message)<<'\n';previous=status.message;}
            if(status.presses!=lastPress){output<<sun::utf8(status.lastKey)<<'\n';lastPress=status.presses;}
            output.flush();
        }
        backend.stop();auto s=backend.status();
        output<<"reports="<<s.reports<<" mapped_presses="<<s.presses<<" injected="<<s.sent<<'\n';
        return s.reports?0:3;
    }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
}

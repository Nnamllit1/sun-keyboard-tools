#include "ipc.hpp"
#include <iostream>
#include <random>
#include <cstring>
#include <optional>
#include <fstream>
#include <aclapi.h>
#include <sddl.h>

namespace {
int checks{};
void require(bool good,const char* text){++checks;if(!good)throw std::runtime_error(text);}
template<class F>void rejects(F f){bool rejected=false;try{f();}catch(const std::exception&){rejected=true;}require(rejected,"Unsafe input accepted");}
}
void pathChecks() {
    require(sun::elevated(),"Path integration checks require elevation");
    auto root=sun::installFolder().parent_path()/(L"Sun Keyboard Tools Security Test-"+std::to_wstring(GetCurrentProcessId()));
    require(!std::filesystem::exists(root),"Fixture already exists");sun::createProtectedDirectory(root);
    struct Cleanup {std::filesystem::path root;~Cleanup(){DeleteFileW((root/L"alias.bin").c_str());DeleteFileW((root/L"file.bin").c_str());RemoveDirectoryW((root/L"link").c_str());RemoveDirectoryW((root/L"weak").c_str());RemoveDirectoryW(root.c_str());}} cleanup{root};
    {auto locks=sun::lockProtected(root,true);require(!locks.empty(),"Protected fixture rejected");}
    {std::ofstream file(root/L"file.bin");file<<"security test fixture";}
    require(CreateHardLinkW((root/L"alias.bin").c_str(),(root/L"file.bin").c_str(),nullptr)!=FALSE,"Cannot create hard-link fixture");
    rejects([&]{sun::lockProtected(root/L"file.bin");});DeleteFileW((root/L"alias.bin").c_str());
    sun::createProtectedDirectory(root/L"weak");
    require(CreateSymbolicLinkW((root/L"link").c_str(),(root/L"weak").c_str(),SYMBOLIC_LINK_FLAG_DIRECTORY)!=FALSE,"Cannot create reparse fixture");
    rejects([&]{sun::lockProtected(root/L"link",true);});
    auto text=L"D:P(A;;FA;;;BA)(A;;FA;;;SY)(A;;GW;;;"+sun::accountSid()+L")";PSECURITY_DESCRIPTOR descriptor{};
    require(ConvertStringSecurityDescriptorToSecurityDescriptorW(text.c_str(),SDDL_REVISION_1,&descriptor,nullptr)!=FALSE,"Create weak fixture ACL");
    BOOL present{},defaulted{};PACL acl{};GetSecurityDescriptorDacl(descriptor,&present,&acl,&defaulted);
    auto code=SetNamedSecurityInfoW((root/L"weak").wstring().data(),SE_FILE_OBJECT,DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,nullptr,nullptr,acl,nullptr);
    LocalFree(descriptor);require(code==ERROR_SUCCESS,"Apply weak fixture ACL");
    rejects([&]{sun::lockProtected(root/L"weak",true);});
    std::cout<<"Protected, hard-link, reparse, and writable-ACL fixture checks passed\n";
}
int main(int argc,char** argv) {
    try {
        if(argc==2 && std::string_view(argv[1])=="--paths"){pathChecks();return 0;}
        sun::Profile profile;profile.vid=0x430;profile.pid=0xa2;profile.mappings={{0x7c,"Copy","Ctrl+C"},{0x7d,"Paste","Ctrl+V"}};
        auto good=sun::wire::request(profile);auto parsed=sun::wire::profile(good);
        require(parsed.mappings.size()==2 && parsed.mappings[0].shortcut.empty(),"Privileged protocol contains a shortcut");
        for(auto mutate:std::vector<std::function<void(sun::wire::Request&)>>{
            [](auto& r){r.signature=0;},[](auto& r){r.revision=2;},[](auto& r){r.operation=99;},
            [](auto& r){r.vid=65536;},[](auto& r){r.pid=0;},[](auto& r){r.endpoint=1;},[](auto& r){r.endpoint=0x91;},
            [](auto& r){r.reportSize=257;},[](auto& r){r.reportSize=0;},[](auto& r){r.keysOffset=UINT32_MAX;},
            [](auto& r){r.keysCount=UINT32_MAX;},[](auto& r){r.keysCount=0;},[](auto& r){r.reportId=257;},
            [](auto& r){r.reportId=1;r.keysOffset=0;},[](auto& r){r.count=33;},[](auto& r){r.count=0;},
            [](auto& r){r.usages[0]=3;},[](auto& r){r.usages[0]=256;},[](auto& r){r.usages[1]=r.usages[0];},
            [](auto& r){r.usages[31]=1;}}) {auto bad=good;mutate(bad);rejects([&]{sun::wire::profile(bad);});}
        // Exercise the decoder with hostile integer values, independently of INI validation.
        std::mt19937 random(0x534b54);
        for(int i=0;i<10000;++i) {
            auto request=good;std::array<uint32_t,sizeof(request)/4> fields{};std::memcpy(fields.data(),&request,sizeof(request));
            fields[random()%fields.size()]=random();std::memcpy(&request,fields.data(),sizeof(request));
            std::optional<sun::Profile> p;try{p=sun::wire::profile(request);}catch(const std::exception&){}
            if(p)require(p->keysOffset+p->keysCount<=p->reportSize && p->mappings.size()<=32,"Layout escaped bounds");
        }
        sun::wire::Frame event{sun::wire::magic,sun::wire::version,3,0x7c};require(sun::wire::valid(event),"Valid event rejected");
        for(auto value:{0u,3u,256u,UINT32_MAX}){event.value=value;require(!sun::wire::valid(event),"Invalid key event accepted");}
        event={sun::wire::magic,sun::wire::version,99,0};require(!sun::wire::valid(event),"Unknown event accepted");
        rejects([]{sun::lockProtected(std::filesystem::current_path(),true);});
        rejects([]{sun::lockProtected(L"relative.exe");});
        rejects([]{sun::lockProtected(L"\\\\server\\share\\fake.exe");});
        std::array<wchar_t,32768> system{};require(GetSystemDirectoryW(system.data(),static_cast<UINT>(system.size()))!=0,"Cannot locate System32");
        auto locks=sun::lockProtected(system.data(),true);require(!locks.empty(),"Protected Windows directory rejected");
        std::cout<<checks<<" security checks passed; 10000 bounded protocol mutations exercised\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}

#include "backend.hpp"
#include <commctrl.h>
#include <shellapi.h>
#include <objbase.h>
#include <algorithm>
#include <fstream>
#include <sstream>

#pragma comment(linker,"\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
namespace {
constexpr wchar_t windowClass[]=L"SunKeyboardTools.Settings";
constexpr UINT trayMessage=WM_APP+1;
constexpr int profileId=100,applyId=101,pauseId=102,monitorId=103,startupId=104,folderId=105,helpId=106,resetId=107,exitId=108;
HWND window{},statusLabel{},lastLabel{},profileBox{},pauseButton{},monitorBox{},startupBox{},countLabel{};
HFONT normalFont{},titleFont{};
std::vector<HWND> rows;
std::vector<std::filesystem::path> profiles;
sun::Profile profile;
size_t selected{};
sun::Backend backend;
bool paused{},monitor{},dirty{};
UINT dpi=96,taskbarCreated{};
int footerY{};
NOTIFYICONDATAW tray{};
int px(int v){return MulDiv(v,dpi,96);}
std::wstring value(HWND h){auto n=GetWindowTextLengthW(h);std::wstring s(n+1,0);GetWindowTextW(h,s.data(),n+1);s.resize(n);return s;}
void error(const std::exception& e){MessageBoxW(window,sun::wide(e.what()).c_str(),L"Sun Keyboard Tools",MB_ICONERROR);}
HWND control(const wchar_t* cls,const std::wstring& text,DWORD style,int x,int y,int w,int h,int id=0){
    auto child=CreateWindowExW(wcscmp(cls,L"EDIT")==0?WS_EX_CLIENTEDGE:0,cls,text.c_str(),WS_CHILD|WS_VISIBLE|style,
        px(x),px(y),px(w),px(h),window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),nullptr,nullptr);
    SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(normalFont),TRUE);return child;
}
void start(){backend.stop();if(!paused)backend.start(profile,monitor);SetWindowTextW(pauseButton,paused?L"Resume remapping":L"Pause remapping");}
void saveSelected(){std::ofstream out(sun::userFolder()/L"selected.txt");out<<sun::utf8(profiles.at(selected).filename().wstring());if(!out)throw std::runtime_error("Cannot save profile selection");}
void loadRows(){
    for(auto h:rows)DestroyWindow(h);rows.clear();
    int nextFooter=170+static_cast<int>((profile.mappings.size()+1)/2)*29+12;
    if(footerY && nextFooter!=footerY){
        struct Shift{int oldY,delta;} shift{px(footerY),px(nextFooter-footerY)};
        EnumChildWindows(window,[](HWND child,LPARAM data)->BOOL{
            auto s=reinterpret_cast<Shift*>(data);RECT r{};GetWindowRect(child,&r);MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&r),2);
            if(r.top>=s->oldY)SetWindowPos(child,nullptr,r.left,r.top+s->delta,0,0,SWP_NOSIZE|SWP_NOZORDER);return TRUE;
        },reinterpret_cast<LPARAM>(&shift));
        RECT rect{0,0,px(700),px(nextFooter+188)};AdjustWindowRectExForDpi(&rect,WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,FALSE,0,dpi);
        SetWindowPos(window,nullptr,0,0,rect.right-rect.left,rect.bottom-rect.top,SWP_NOMOVE|SWP_NOZORDER);
    }
    footerY=nextFooter;
    for(size_t i=0;i<profile.mappings.size();++i){
        // Two columns allow up to 32 extra keys while keeping the window compact.
        int col=static_cast<int>(i%2),row=static_cast<int>(i/2);
        auto& m=profile.mappings[i];
        rows.push_back(control(L"STATIC",sun::wide(m.name),0,24+col*330,174+row*29,100,24));
        auto edit=control(L"COMBOBOX",L"",CBS_DROPDOWN|WS_VSCROLL|WS_TABSTOP,128+col*330,170+row*29,210,210,200+static_cast<int>(i));
        for(auto text:{L"Disabled",L"Ctrl+C",L"Ctrl+V",L"Ctrl+X",L"Ctrl+Z",L"Ctrl+Y",L"Ctrl+F",L"Ctrl+O",L"Alt+Tab",L"Alt+Enter",L"Esc",L"F1"})
            SendMessageW(edit,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(text));
        SendMessageW(edit,CB_LIMITTEXT,80,0);SetWindowTextW(edit,sun::wide(m.shortcut).c_str());rows.push_back(edit);
    }
    dirty=false;
}
void addTray(){
    tray={sizeof(tray)};tray.hWnd=window;tray.uID=1;tray.uFlags=NIF_MESSAGE|NIF_ICON|NIF_TIP;
    tray.uCallbackMessage=trayMessage;tray.hIcon=LoadIconW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(1));
    wcscpy_s(tray.szTip,L"Sun Keyboard Tools - extra key remapping");Shell_NotifyIconW(NIM_ADD,&tray);
    tray.uVersion=NOTIFYICON_VERSION_4;Shell_NotifyIconW(NIM_SETVERSION,&tray);
}
void show(){ShowWindow(window,SW_RESTORE);SetForegroundWindow(window);}
bool discard(){return !dirty || MessageBoxW(window,L"Discard your unsaved shortcut changes?",L"Sun Keyboard Tools",MB_YESNO|MB_ICONQUESTION)==IDYES;}
void command(int id){
    if(id>=200 && id<232){dirty=true;return;}
    switch(id){
    case applyId:{
        auto changed=profile;
        for(size_t i=0;i<changed.mappings.size();++i){auto text=sun::utf8(value(rows.at(i*2+1)));sun::parseChord(text);changed.mappings[i].shortcut=text;}
        sun::writeProfile(profiles.at(selected),changed);
        auto temp=profiles.at(selected);temp+=L".tmp";
        if(!MoveFileExW(temp.c_str(),profiles.at(selected).c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("Cannot replace saved profile: "+sun::utf8(sun::winError()));
        profile=changed;dirty=false;start();break;}
    case pauseId:paused=!paused;start();break;
    case monitorId:monitor=SendMessageW(monitorBox,BM_GETCHECK,0,0)==BST_CHECKED;start();break;
    case startupId:{
        try{sun::setStartup(SendMessageW(startupBox,BM_GETCHECK,0,0)==BST_CHECKED,sun::executable());}
        catch(...){SendMessageW(startupBox,BM_SETCHECK,sun::hasStartup()?BST_CHECKED:BST_UNCHECKED,0);throw;}break;}
    case folderId:ShellExecuteW(window,L"open",(sun::userFolder()/L"profiles").c_str(),nullptr,nullptr,SW_SHOWNORMAL);break;
    case helpId:ShellExecuteW(window,L"open",(sun::executable().parent_path()/L"README.html").c_str(),nullptr,nullptr,SW_SHOWNORMAL);break;
    case resetId:
        if(MessageBoxW(window,L"Replace the displayed shortcuts with this profile's bundled defaults? Choose Save shortcuts to keep the change.",L"Restore defaults",MB_YESNO|MB_ICONQUESTION)==IDYES){
            auto path=sun::executable().parent_path()/L"profiles"/profiles.at(selected).filename();
            auto defaults=sun::readProfile(path);profile=defaults;loadRows();dirty=true;
        }break;
    case exitId:if(discard())DestroyWindow(window);break;
    }
}
LRESULT CALLBACK proc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp){
    try{
        if(msg==taskbarCreated && taskbarCreated){addTray();return 0;}
        switch(msg){
        case WM_CTLCOLORSTATIC:
            SetBkMode(reinterpret_cast<HDC>(wp),TRANSPARENT);
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
        case WM_CLOSE:ShowWindow(hwnd,SW_HIDE);return 0;
        case WM_COMMAND:
            if(LOWORD(wp)==profileId && HIWORD(wp)==CBN_SELCHANGE){
                auto next=static_cast<size_t>(SendMessageW(profileBox,CB_GETCURSEL,0,0));
                if(!discard()){SendMessageW(profileBox,CB_SETCURSEL,selected,0);return 0;}
                sun::Profile newProfile;
                try{newProfile=sun::readProfile(profiles.at(next));}catch(...){SendMessageW(profileBox,CB_SETCURSEL,selected,0);throw;}
                profile=std::move(newProfile);selected=next;loadRows();saveSelected();start();return 0;
            }
            if(LOWORD(wp)>=200 && HIWORD(wp)!=CBN_EDITCHANGE && HIWORD(wp)!=CBN_SELCHANGE)return 0;
            command(LOWORD(wp));return 0;
        case WM_TIMER:{
            auto state=backend.status();
            SetWindowTextW(statusLabel,paused?L"Paused. Use Resume remapping when you are ready.":state.message.c_str());
            SetWindowTextW(lastLabel,state.lastKey.c_str());
            auto counts=L"Extra keys: "+std::to_wstring(state.presses)+L"   Shortcuts sent: "+std::to_wstring(state.sent)+L"   Skipped: "+std::to_wstring(state.skipped);
            SetWindowTextW(countLabel,counts.c_str());return 0;}
        case trayMessage:
            if(LOWORD(lp)==NIN_SELECT || LOWORD(lp)==NIN_KEYSELECT){show();return 0;}
            if(LOWORD(lp)==WM_CONTEXTMENU){
                POINT pt{};GetCursorPos(&pt);auto menu=CreatePopupMenu();
                AppendMenuW(menu,MF_STRING,1,L"Open Sun Keyboard Tools");AppendMenuW(menu,MF_STRING,pauseId,paused?L"Resume":L"Pause");AppendMenuW(menu,MF_STRING,exitId,L"Exit");
                SetForegroundWindow(hwnd);auto id=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,pt.x,pt.y,0,hwnd,nullptr);DestroyMenu(menu);
                if(id==1)show();else if(id)command(id);return 0;
            }break;
        case WM_DESTROY:KillTimer(hwnd,1);backend.stop();Shell_NotifyIconW(NIM_DELETE,&tray);PostQuitMessage(0);return 0;
        }
    }catch(const std::exception& e){error(e);}
    return DefWindowProcW(hwnd,msg,wp,lp);
}
void initProfiles(){
    auto folder=sun::userFolder()/L"profiles";std::filesystem::create_directories(folder);
    auto bundled=sun::executable().parent_path()/L"profiles";
    for(const auto& file:std::filesystem::directory_iterator(bundled))if(file.path().extension()==L".ini")
        std::filesystem::copy_file(file.path(),folder/file.path().filename(),std::filesystem::copy_options::skip_existing);
    for(const auto& file:std::filesystem::directory_iterator(folder))if(file.path().extension()==L".ini")profiles.push_back(file.path());
    std::sort(profiles.begin(),profiles.end());if(profiles.empty())throw std::runtime_error("No keyboard profiles found");
    std::ifstream in(sun::userFolder()/L"selected.txt");std::string chosen;std::getline(in,chosen);
    for(size_t i=0;i<profiles.size();++i)if(sun::utf8(profiles[i].filename().wstring())==chosen)selected=i;
    profile=sun::readProfile(profiles[selected]);
}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int){
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32|LOAD_LIBRARY_SEARCH_APPLICATION_DIR);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    try{
        int argc{};auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);
        bool background=false;for(int i=1;i<argc;++i)if(std::wstring_view(argv[i])==L"--background")background=true;
        LocalFree(argv);
        if(!sun::elevated())throw std::runtime_error("Run Sun Keyboard Tools as administrator to access USBPcap.");
        sun::Handle singleton(CreateMutexW(nullptr,TRUE,L"Local\\SunKeyboardTools.App"));
        if(GetLastError()==ERROR_ALREADY_EXISTS){auto other=FindWindowW(windowClass,nullptr);if(other){ShowWindow(other,SW_RESTORE);SetForegroundWindow(other);}return 0;}
        initProfiles();
        INITCOMMONCONTROLSEX cc{sizeof(cc),ICC_STANDARD_CLASSES};InitCommonControlsEx(&cc);
        dpi=GetDpiForSystem();normalFont=CreateFontW(-px(14),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
        titleFont=CreateFontW(-px(25),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
        WNDCLASSW cls{};cls.lpfnWndProc=proc;cls.hInstance=instance;cls.lpszClassName=windowClass;cls.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);cls.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(1));
        if(!RegisterClassW(&cls))throw std::runtime_error("Cannot register settings window: "+sun::utf8(sun::winError()));
        RECT size{0,0,px(700),px(820)};AdjustWindowRectExForDpi(&size,WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,FALSE,0,dpi);
        footerY=170+static_cast<int>((profile.mappings.size()+1)/2)*29+12;
        size={0,0,px(700),px(footerY+188)};AdjustWindowRectExForDpi(&size,WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,FALSE,0,dpi);
        window=CreateWindowW(windowClass,L"Sun Keyboard Tools",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,CW_USEDEFAULT,CW_USEDEFAULT,size.right-size.left,size.bottom-size.top,nullptr,nullptr,instance,nullptr);
        if(!window)throw std::runtime_error("Cannot create settings window: "+sun::utf8(sun::winError()));
        auto title=control(L"STATIC",L"Your Sun keys, useful again.",0,24,18,650,38);SendMessageW(title,WM_SETFONT,reinterpret_cast<WPARAM>(titleFont),TRUE);
        statusLabel=control(L"STATIC",L"Starting...",0,24,63,652,48);
        control(L"STATIC",L"Keyboard profile",0,24,121,130,22);
        profileBox=control(L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP,160,116,330,230,profileId);
        for(auto& path:profiles)SendMessageW(profileBox,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(path.stem().c_str()));
        SendMessageW(profileBox,CB_SETCURSEL,selected,0);
        control(L"BUTTON",L"Profile folder",WS_TABSTOP,506,116,170,28,folderId);
        loadRows();
        control(L"STATIC",L"Choose a shortcut or type one, such as Ctrl+Shift+Z. Use Disabled to ignore a key.",0,24,footerY,652,24);
        control(L"BUTTON",L"Save shortcuts",WS_TABSTOP|BS_DEFPUSHBUTTON,24,footerY+34,156,32,applyId);
        control(L"BUTTON",L"Restore defaults",WS_TABSTOP,192,footerY+34,156,32,resetId);
        pauseButton=control(L"BUTTON",L"Pause remapping",WS_TABSTOP,360,footerY+34,156,32,pauseId);
        control(L"BUTTON",L"Help",WS_TABSTOP,528,footerY+34,148,32,helpId);
        monitorBox=control(L"BUTTON",L"Key monitor (no actions)",BS_AUTOCHECKBOX|WS_TABSTOP,24,footerY+78,280,24,monitorId);
        startupBox=control(L"BUTTON",L"Start automatically when I sign in",BS_AUTOCHECKBOX|WS_TABSTOP,320,footerY+78,356,24,startupId);
        SendMessageW(startupBox,BM_SETCHECK,sun::hasStartup()?BST_CHECKED:BST_UNCHECKED,0);
        lastLabel=control(L"STATIC",L"No extra key received yet",0,24,footerY+111,652,24);
        countLabel=control(L"STATIC",L"",0,24,footerY+141,652,24);
        taskbarCreated=RegisterWindowMessageW(L"TaskbarCreated");addTray();SetTimer(window,1,500,nullptr);start();
        if(!background)ShowWindow(window,SW_SHOW);
        MSG msg{};while(GetMessageW(&msg,nullptr,0,0)>0){if(!IsDialogMessageW(window,&msg)){TranslateMessage(&msg);DispatchMessageW(&msg);}}
        DeleteObject(normalFont);DeleteObject(titleFont);CoUninitialize();return 0;
    }catch(const std::exception& e){error(e);CoUninitialize();return 1;}
}

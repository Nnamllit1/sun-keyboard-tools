#include "backend.hpp"
#include "appearance.hpp"
#include <commctrl.h>
#include <shellapi.h>
#include <objbase.h>
#include <algorithm>
#include <fstream>
#include <sstream>

#pragma comment(linker,"\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
namespace {
constexpr wchar_t windowClass[]=L"SunKeyboardTools.Settings";
constexpr wchar_t utilityClass[]=L"SunKeyboardTools.Utility";
constexpr UINT trayMessage=WM_APP+1;
constexpr int profileId=100,applyId=101,pauseId=102,monitorId=103,startupId=104,folderId=105,helpId=106,resetId=107,exitId=108;
constexpr int settingsId=109,activityId=110;
HWND window{},statusLabel{},lastLabel{},profileBox{},pauseButton{},monitorBox{},startupBox{},stateLabel{},saveLabel{},saveButton{};
HWND settingsWindow{},activityWindow{},activityStateLabel{},profileTitle{};
HWND counterLabels[3]{};
sun::ui::Appearance appearance;
std::vector<HWND> rows;
std::vector<HWND> layoutControls;
std::vector<std::filesystem::path> profiles;
sun::Profile profile;
size_t selected{};
sun::Backend backend;
bool paused{},monitor{},dirty{};
UINT dpi=96,taskbarCreated{};
int footerY{},contentHeight{},scrollY{},wheelDelta{};
int activeMapping{-1};
ULONGLONG highlightUntil{};
sun::Status currentStatus;
NOTIFYICONDATAW tray{};
int px(int v){return MulDiv(v,dpi,96);}
std::wstring value(HWND h){auto n=GetWindowTextLengthW(h);std::wstring s(n+1,0);GetWindowTextW(h,s.data(),n+1);s.resize(n);return s;}
void error(const std::exception& e,HWND owner=nullptr){MessageBoxW(owner?owner:window,sun::wide(e.what()).c_str(),L"Sun Keyboard Tools",MB_ICONERROR);}
HWND control(const wchar_t* cls,const std::wstring& text,DWORD style,int x,int y,int w,int h,int id=0,HWND parent=nullptr,UINT role=0){
    if(!parent)parent=window;
    auto child=CreateWindowExW(wcscmp(cls,L"EDIT")==0?WS_EX_CLIENTEDGE:0,cls,text.c_str(),WS_CHILD|WS_VISIBLE|style|(wcscmp(cls,L"BUTTON")==0?BS_NOTIFY:0),
        px(x),px(y)-(parent==window?scrollY:0),px(w),px(h),parent,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),nullptr,nullptr);
    SetPropW(child,L"Sun.TextRole",reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(role)));
    SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(appearance.normal),TRUE);return child;
}
HWND label(const std::wstring& text,int x,int y,int w,int h,HFONT font=nullptr,UINT role=0,HWND parent=nullptr){
    auto child=control(L"STATIC",text,SS_NOPREFIX,x,y,w,h,0,parent);
    if(font)SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);
    SetPropW(child,L"Sun.TextRole",reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(role)));return child;
}
void setText(HWND h,const std::wstring& text){if(h && value(h)!=text)SetWindowTextW(h,text.c_str());}
RECT rect(int x,int y,int w,int h){return {px(x),px(y),px(x+w),px(y+h)};}
void scrollTo(int position){
    RECT client{};GetClientRect(window,&client);
    auto next=std::clamp(position,0,std::max(0,px(contentHeight)-static_cast<int>(client.bottom)));
    int delta=scrollY-next;scrollY=next;
    if(delta)ScrollWindowEx(window,0,delta,nullptr,nullptr,nullptr,nullptr,SW_SCROLLCHILDREN|SW_INVALIDATE|SW_ERASE);
    SCROLLINFO info{sizeof(info),SIF_RANGE|SIF_PAGE|SIF_POS,0,px(contentHeight)-1,static_cast<UINT>(client.bottom),scrollY,0};
    SetScrollInfo(window,SB_VERT,&info,TRUE);
}
void ensureVisible(HWND child){
    if(!child || !IsChild(window,child))return;
    RECT r{},client{};GetWindowRect(child,&r);MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&r),2);GetClientRect(window,&client);
    // A combo's window rectangle includes the dropdown; only keep its edit in view.
    int bottom=std::min(static_cast<int>(r.bottom),static_cast<int>(r.top)+px(36));
    if(r.top<px(12))scrollTo(scrollY+r.top-px(12));
    else if(bottom>client.bottom-px(12))scrollTo(scrollY+bottom-client.bottom+px(12));
}
void saveState(bool changed,const wchar_t* message=nullptr){
    dirty=changed;
    setText(saveLabel,message?message:(dirty?L"Unsaved changes":L"All changes saved"));
    EnableWindow(saveButton,dirty);InvalidateRect(saveLabel,nullptr,TRUE);
}
void updateStatus(){
    auto state=backend.status();
    setText(stateLabel,paused?L"Remapping paused":state.connected?(monitor?L"Monitor mode":L"Ready to remap"):L"Waiting for keyboard");
    setText(statusLabel,paused?L"Your shortcuts are paused. Resume when you're ready.":state.message);
    setText(activityStateLabel,paused?L"Remapping is paused":monitor?L"Test mode: shortcuts are not sent":state.connected?L"Remapping is active":L"Waiting for your keyboard");
    setText(lastLabel,state.lastKey);
    uint64_t counts[]{state.presses,state.sent,state.skipped};
    for(int i=0;i<3;++i)setText(counterLabels[i],std::to_wstring(counts[i]));
    if(state.presses!=currentStatus.presses && state.presses){
        for(size_t i=0;i<profile.mappings.size();++i)if(state.lastKey.starts_with(sun::wide(profile.mappings[i].name)+L" -> ")){
            if(activeMapping>=0)InvalidateRect(rows[static_cast<size_t>(activeMapping)*2],nullptr,TRUE);
            activeMapping=static_cast<int>(i);highlightUntil=GetTickCount64()+850;
            InvalidateRect(rows[i*2],nullptr,TRUE);break;
        }
    }
    if(activeMapping>=0 && GetTickCount64()>=highlightUntil){InvalidateRect(rows[static_cast<size_t>(activeMapping)*2],nullptr,TRUE);activeMapping=-1;}
    if(state.connected!=currentStatus.connected){InvalidateRect(stateLabel,nullptr,TRUE);if(activityStateLabel)InvalidateRect(activityStateLabel,nullptr,TRUE);InvalidateRect(window,nullptr,FALSE);}
    currentStatus=std::move(state);
}
void start(){
    backend.stop();if(!paused)backend.start(profile,monitor);
    setText(pauseButton,paused?L"Resume remapping":L"Pause remapping");
    updateStatus();InvalidateRect(stateLabel,nullptr,TRUE);InvalidateRect(window,nullptr,FALSE);
    if(activityStateLabel)InvalidateRect(activityStateLabel,nullptr,TRUE);
}
void card(HDC dc,int x,int y,int w,int h){
    appearance.round(dc,rect(x,y+2,w,h),appearance.shadow,appearance.shadow,16);
    appearance.round(dc,rect(x,y,w,h),appearance.panel,appearance.border,16);
}
void paint(HDC dc){
    RECT client{};GetClientRect(window,&client);FillRect(dc,&client,appearance.canvasBrush);
    auto saved=SaveDC(dc);SetViewportOrgEx(dc,0,-scrollY,nullptr);
    card(dc,24,112,576,footerY+86-112);
    // The keyboard mark echoes the physical keys without decorative controls.
    appearance.round(dc,rect(24,28,48,48),appearance.accent,appearance.accent,14);
    for(int y=0;y<3;++y)for(int x=0;x<3;++x)
        appearance.round(dc,rect(34+x*10,39+y*9,7,6),appearance.highContrast?GetSysColor(COLOR_HIGHLIGHTTEXT):RGB(255,255,255),appearance.highContrast?GetSysColor(COLOR_HIGHLIGHTTEXT):RGB(255,255,255),2);
    COLORREF stateColor=paused||!currentStatus.connected?appearance.amber:monitor?appearance.accent:appearance.green;
    appearance.round(dc,rect(24,footerY+111,9,9),stateColor,stateColor,9);
    RestoreDC(dc,saved);
}
LRESULT drawButton(NMCUSTOMDRAW* draw){
    auto id=static_cast<int>(draw->hdr.idFrom);
    if(draw->dwDrawStage!=CDDS_PREPAINT || id==monitorId || id==startupId || appearance.highContrast)return CDRF_DODEFAULT;
    bool primary=id==applyId,disabled=!IsWindowEnabled(draw->hdr.hwndFrom);
    bool hot=(draw->uItemState&CDIS_HOT)!=0,down=(draw->uItemState&CDIS_SELECTED)!=0;
    auto canvas=(reinterpret_cast<UINT_PTR>(GetPropW(draw->hdr.hwndFrom,L"Sun.TextRole"))&4)!=0;
    auto r=draw->rc;FillRect(draw->hdc,&r,canvas?appearance.canvasBrush:appearance.panelBrush);
    auto face=r;face.right-=px(1);face.bottom-=px(2);if(down)OffsetRect(&face,0,px(1));
    auto shade=face;OffsetRect(&shade,0,px(2));appearance.round(draw->hdc,shade,appearance.shadow,appearance.shadow,9);
    auto fill=disabled?appearance.canvas:primary?(hot||down?appearance.accentHot:appearance.accent):hot?appearance.tint:appearance.panel;
    appearance.round(draw->hdc,face,fill,primary&&!disabled?fill:appearance.border,9);
    appearance.text(draw->hdc,value(draw->hdr.hwndFrom),face,appearance.normal,
        disabled?appearance.muted:primary?RGB(255,255,255):appearance.ink,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    if((draw->uItemState&CDIS_FOCUS) && !(SendMessageW(draw->hdr.hwndFrom,WM_QUERYUISTATE,0,0)&UISF_HIDEFOCUS)){
        InflateRect(&face,-px(4),-px(4));DrawFocusRect(draw->hdc,&face);
    }
    return CDRF_SKIPDEFAULT;
}
void saveSelected(){std::ofstream out(sun::userFolder()/L"selected.txt");out<<sun::utf8(profiles.at(selected).filename().wstring());if(!out)throw std::runtime_error("Cannot save profile selection");}
void orderTabs(){
    auto move=[](HWND child){if(child)SetWindowPos(child,HWND_BOTTOM,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);};
    for(auto child:rows)move(child);
    for(auto id:{applyId,pauseId,settingsId})move(GetDlgItem(window,id));
}
void loadRows(){
    for(auto h:rows)DestroyWindow(h);rows.clear();
    for(auto h:layoutControls)DestroyWindow(h);layoutControls.clear();
    activeMapping=-1;
    footerY=196+static_cast<int>((profile.mappings.size()+1)/2)*42+12;
    contentHeight=footerY+190;
    setText(profileTitle,sun::wide(profile.name));
    for(size_t i=0;i<profile.mappings.size();++i){
        int col=static_cast<int>(i%2),row=static_cast<int>(i/2);
        auto& m=profile.mappings[i];
        rows.push_back(control(L"STATIC",sun::wide(m.name),SS_OWNERDRAW,44+col*276,196+row*42,78,32,400+static_cast<int>(i)));
        auto edit=control(L"COMBOBOX",L"",CBS_DROPDOWN|WS_VSCROLL|WS_TABSTOP,130+col*276,197+row*42,168,230,200+static_cast<int>(i));
        SendMessageW(edit,CB_SETITEMHEIGHT,static_cast<WPARAM>(-1),px(22));
        SendMessageW(edit,CB_SETITEMHEIGHT,0,px(26));
        for(auto text:{L"Disabled",L"Ctrl+C",L"Ctrl+V",L"Ctrl+X",L"Ctrl+Z",L"Ctrl+Y",L"Ctrl+F",L"Ctrl+O",L"Alt+Tab",L"Alt+Enter",L"Esc",L"F1"})
            SendMessageW(edit,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(text));
        SendMessageW(edit,CB_LIMITTEXT,80,0);SetWindowTextW(edit,sun::wide(m.shortcut).c_str());rows.push_back(edit);
    }
    layoutControls.push_back(label(L"Choose a shortcut, or type your own (e.g. Ctrl+Shift+Z).",44,footerY,536,20,appearance.caption,1));
    saveButton=control(L"BUTTON",L"Save shortcuts",WS_TABSTOP|BS_DEFPUSHBUTTON,44,footerY+30,164,36,applyId);layoutControls.push_back(saveButton);
    saveLabel=label(L"",228,footerY+41,346,20,appearance.caption,1);layoutControls.push_back(saveLabel);
    stateLabel=label(L"",44,footerY+104,370,26,appearance.heading,4);layoutControls.push_back(stateLabel);
    pauseButton=control(L"BUTTON",paused?L"Resume remapping":L"Pause remapping",WS_TABSTOP,432,footerY+99,168,32,pauseId,nullptr,4);layoutControls.push_back(pauseButton);
    statusLabel=label(L"",24,footerY+142,576,40,appearance.normal,1|4);layoutControls.push_back(statusLabel);
    saveState(false);
    // Keep even extended profiles reachable on small screens and at large DPI.
    MONITORINFO monitorInfo{sizeof(monitorInfo)};GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&monitorInfo);
    RECT size{0,0,px(624),px(contentHeight)};
    AdjustWindowRectExForDpi(&size,WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX|WS_VSCROLL,FALSE,WS_EX_CONTROLPARENT,dpi);
    int height=std::min(static_cast<int>(size.bottom-size.top),static_cast<int>(monitorInfo.rcWork.bottom-monitorInfo.rcWork.top)-px(24));
    RECT current{};GetWindowRect(window,&current);
    int top=std::clamp(static_cast<int>(current.top),static_cast<int>(monitorInfo.rcWork.top),static_cast<int>(monitorInfo.rcWork.bottom)-height);
    SetWindowPos(window,nullptr,current.left,top,size.right-size.left,height,SWP_NOZORDER|SWP_NOACTIVATE);
    scrollTo(0);orderTabs();InvalidateRect(window,nullptr,TRUE);
}
void addTray(){
    tray={sizeof(tray)};tray.hWnd=window;tray.uID=1;tray.uFlags=NIF_MESSAGE|NIF_ICON|NIF_TIP;
    tray.uCallbackMessage=trayMessage;tray.hIcon=LoadIconW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(1));
    wcscpy_s(tray.szTip,L"Sun Keyboard Tools - extra key remapping");Shell_NotifyIconW(NIM_ADD,&tray);
    tray.uVersion=NOTIFYICON_VERSION_4;Shell_NotifyIconW(NIM_SETVERSION,&tray);
}
void show(){ShowWindow(window,SW_RESTORE);SetForegroundWindow(window);}
bool discard(HWND owner=nullptr){return !dirty || MessageBoxW(owner?owner:window,L"Discard your unsaved shortcut changes?",L"Sun Keyboard Tools",MB_YESNO|MB_ICONQUESTION)==IDYES;}
void openSettings();
void openActivity();
void closeActivity(){
    if(activityWindow)ShowWindow(activityWindow,SW_HIDE);
    if(monitor){monitor=false;SendMessageW(monitorBox,BM_SETCHECK,BST_UNCHECKED,0);start();}
}
void command(int id,HWND owner=nullptr){
    if(id>=200 && id<232){saveState(true);return;}
    switch(id){
    case applyId:{
        auto changed=profile;
        for(size_t i=0;i<changed.mappings.size();++i){auto text=sun::utf8(value(rows.at(i*2+1)));sun::parseChord(text);changed.mappings[i].shortcut=text;}
        sun::writeProfile(profiles.at(selected),changed);
        auto temp=profiles.at(selected);temp+=L".tmp";
        if(!MoveFileExW(temp.c_str(),profiles.at(selected).c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("Cannot replace saved profile: "+sun::utf8(sun::winError()));
        profile=changed;saveState(false,L"Shortcuts saved");start();break;}
    case pauseId:paused=!paused;start();break;
    case monitorId:monitor=SendMessageW(monitorBox,BM_GETCHECK,0,0)==BST_CHECKED;start();break;
    case startupId:{
        try{sun::setStartup(SendMessageW(startupBox,BM_GETCHECK,0,0)==BST_CHECKED,sun::executable());}
        catch(...){SendMessageW(startupBox,BM_SETCHECK,sun::hasStartup()?BST_CHECKED:BST_UNCHECKED,0);throw;}break;}
    case folderId:ShellExecuteW(window,L"open",(sun::userFolder()/L"profiles").c_str(),nullptr,nullptr,SW_SHOWNORMAL);break;
    case helpId:ShellExecuteW(window,L"open",(sun::executable().parent_path()/L"README.html").c_str(),nullptr,nullptr,SW_SHOWNORMAL);break;
    case settingsId:openSettings();break;
    case activityId:openActivity();break;
    case resetId:
        if(MessageBoxW(owner?owner:window,L"Replace the displayed shortcuts with this profile's bundled defaults? Choose Save shortcuts in the main window to keep the change.",L"Restore defaults",MB_YESNO|MB_ICONQUESTION)==IDYES){
            auto path=sun::executable().parent_path()/L"profiles"/profiles.at(selected).filename();
            auto defaults=sun::readProfile(path);profile=defaults;loadRows();saveState(true);updateStatus();show();
        }break;
    case exitId:if(discard())DestroyWindow(window);break;
    }
}
LRESULT controlColors(WPARAM wp,LPARAM lp){
    auto child=reinterpret_cast<HWND>(lp);
    auto role=reinterpret_cast<UINT_PTR>(GetPropW(child,L"Sun.TextRole"));
    auto color=role&1?appearance.muted:role&2&&!appearance.highContrast?appearance.accent:appearance.ink;
    if(child==stateLabel || child==activityStateLabel)color=paused||!currentStatus.connected?appearance.amber:monitor?appearance.accent:appearance.green;
    if(child==saveLabel && dirty)color=appearance.amber;
    SetTextColor(reinterpret_cast<HDC>(wp),color);SetBkMode(reinterpret_cast<HDC>(wp),TRANSPARENT);
    SetBkColor(reinterpret_cast<HDC>(wp),role&4?appearance.canvas:appearance.panel);
    return reinterpret_cast<LRESULT>(role&4?appearance.canvasBrush:appearance.panelBrush);
}
void paintUtility(HWND hwnd,HDC dc){
    RECT client{};GetClientRect(hwnd,&client);FillRect(dc,&client,appearance.canvasBrush);
    if(GetWindowLongPtrW(hwnd,GWLP_USERDATA)==settingsId){card(dc,24,100,440,82);card(dc,24,198,440,176);}
    else card(dc,24,164,440,136);
}
LRESULT CALLBACK utilityProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp){
    try{
        switch(msg){
        case WM_NCCREATE:SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));break;
        case WM_ERASEBKGND:return 1;
        case WM_PAINT:{PAINTSTRUCT ps{};auto dc=BeginPaint(hwnd,&ps);paintUtility(hwnd,dc);EndPaint(hwnd,&ps);return 0;}
        case WM_PRINTCLIENT:paintUtility(hwnd,reinterpret_cast<HDC>(wp));return 0;
        case WM_NOTIFY:if(reinterpret_cast<NMHDR*>(lp)->code==NM_CUSTOMDRAW)return drawButton(reinterpret_cast<NMCUSTOMDRAW*>(lp));break;
        case WM_CTLCOLORBTN:case WM_CTLCOLORSTATIC:case WM_CTLCOLOREDIT:case WM_CTLCOLORLISTBOX:return controlColors(wp,lp);
        case WM_COMMAND:
            if((LOWORD(wp)==IDOK || LOWORD(wp)==IDCANCEL) && HIWORD(wp)==BN_CLICKED){SendMessageW(hwnd,WM_CLOSE,0,0);return 0;}
            return SendMessageW(window,msg,wp,lp);
        case WM_CLOSE:
            if(hwnd==activityWindow)closeActivity();else ShowWindow(hwnd,SW_HIDE);
            return 0;
        case WM_SYSCOLORCHANGE:case WM_SETTINGCHANGE:
            SendMessageW(window,msg,wp,lp);return 0;
        }
    }catch(const std::exception& e){error(e,hwnd);}
    return DefWindowProcW(hwnd,msg,wp,lp);
}
HWND utilityWindow(const wchar_t* title,int kind,int height){
    RECT size{0,0,px(488),px(height)};
    DWORD style=WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_CLIPCHILDREN;
    AdjustWindowRectExForDpi(&size,style,FALSE,WS_EX_CONTROLPARENT,dpi);
    RECT owner{};GetWindowRect(window,&owner);
    MONITORINFO info{sizeof(info)};GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&info);
    int w=size.right-size.left,h=size.bottom-size.top;
    int x=std::clamp(static_cast<int>(owner.left)+px(40),static_cast<int>(info.rcWork.left),std::max(static_cast<int>(info.rcWork.left),static_cast<int>(info.rcWork.right)-w));
    int y=std::clamp(static_cast<int>(owner.top)+px(40),static_cast<int>(info.rcWork.top),std::max(static_cast<int>(info.rcWork.top),static_cast<int>(info.rcWork.bottom)-h));
    auto result=CreateWindowExW(WS_EX_CONTROLPARENT,utilityClass,title,style,x,y,w,h,window,nullptr,GetModuleHandleW(nullptr),reinterpret_cast<LPVOID>(static_cast<INT_PTR>(kind)));
    if(!result)throw std::runtime_error("Cannot open settings window: "+sun::utf8(sun::winError()));
    return result;
}
void openSettings(){
    if(!settingsWindow){
        settingsWindow=utilityWindow(L"Settings - Sun Keyboard Tools",settingsId,512);
        label(L"Settings",24,22,440,36,appearance.title,4,settingsWindow);
        label(L"Startup, keyboard profile, and tools.",24,64,440,24,appearance.normal,1|4,settingsWindow);
        startupBox=control(L"BUTTON",L"Start when I sign in",BS_AUTOCHECKBOX|WS_TABSTOP,44,117,400,24,startupId,settingsWindow);
        label(L"Runs quietly in the notification area.",44,151,400,18,appearance.caption,1,settingsWindow);
        label(L"Keyboard profile",44,218,400,24,appearance.heading,0,settingsWindow);
        profileBox=control(L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP,44,251,400,230,profileId,settingsWindow);
        SendMessageW(profileBox,CB_SETITEMHEIGHT,static_cast<WPARAM>(-1),px(23));
        std::vector<std::wstring> names;
        for(auto& path:profiles){
            auto name=path.stem().wstring();
            try{name=sun::wide(sun::readProfile(path).name);}catch(const std::exception&){}
            names.push_back(std::move(name));
        }
        for(size_t i=0;i<names.size();++i){
            auto name=names[i];
            // File names distinguish multiple mappings with the same name.
            if(std::count(names.begin(),names.end(),name)>1)name+=L" ("+profiles[i].stem().wstring()+L")";
            SendMessageW(profileBox,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(name.c_str()));
        }
        control(L"BUTTON",L"Profile folder",WS_TABSTOP,44,297,192,32,folderId,settingsWindow);
        control(L"BUTTON",L"Restore defaults",WS_TABSTOP,252,297,192,32,resetId,settingsWindow);
        label(L"Restored shortcuts can be reviewed before saving.",44,344,400,18,appearance.caption,1,settingsWindow);
        control(L"BUTTON",L"Key monitor...",WS_TABSTOP,24,396,212,36,activityId,settingsWindow,4);
        control(L"BUTTON",L"Open help",WS_TABSTOP,252,396,212,36,helpId,settingsWindow,4);
        control(L"BUTTON",L"Done",WS_TABSTOP|BS_DEFPUSHBUTTON,356,456,108,34,IDOK,settingsWindow,4);
    }
    SendMessageW(startupBox,BM_SETCHECK,sun::hasStartup()?BST_CHECKED:BST_UNCHECKED,0);
    SendMessageW(profileBox,CB_SETCURSEL,selected,0);
    ShowWindow(settingsWindow,SW_SHOWNORMAL);SetForegroundWindow(settingsWindow);SetFocus(startupBox);
}
void openActivity(){
    if(!activityWindow){
        activityWindow=utilityWindow(L"Key Monitor - Sun Keyboard Tools",activityId,374);
        label(L"Key monitor",24,22,440,36,appearance.title,4,activityWindow);
        label(L"Check the extra keys on your keyboard.",24,64,440,24,appearance.normal,1|4,activityWindow);
        activityStateLabel=label(L"",24,99,440,24,appearance.heading,4,activityWindow);
        monitorBox=control(L"BUTTON",L"Test keys without sending shortcuts",BS_AUTOCHECKBOX|WS_TABSTOP,24,130,440,24,monitorId,activityWindow,4);
        const wchar_t* names[]{L"Key presses",L"Shortcuts sent",L"Skipped"};
        for(int i=0;i<3;++i){
            counterLabels[i]=label(L"0",44+i*140,182,120,34,appearance.number,2,activityWindow);
            label(names[i],44+i*140,223,120,18,appearance.caption,1,activityWindow);
        }
        lastLabel=label(L"",44,256,400,34,appearance.caption,1,activityWindow);
        label(L"Closing this window ends test mode.",24,328,320,22,appearance.caption,1|4,activityWindow);
        control(L"BUTTON",L"Done",WS_TABSTOP|BS_DEFPUSHBUTTON,356,318,108,34,IDOK,activityWindow,4);
    }
    SendMessageW(monitorBox,BM_SETCHECK,monitor?BST_CHECKED:BST_UNCHECKED,0);updateStatus();
    ShowWindow(activityWindow,SW_SHOWNORMAL);SetForegroundWindow(activityWindow);SetFocus(monitorBox);
}
LRESULT CALLBACK proc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp){
    try{
        if(msg==taskbarCreated && taskbarCreated){addTray();return 0;}
        switch(msg){
        case WM_ERASEBKGND:return 1;
        case WM_PAINT:{
            PAINTSTRUCT ps{};auto dc=BeginPaint(hwnd,&ps);paint(dc);EndPaint(hwnd,&ps);return 0;}
        case WM_PRINTCLIENT:paint(reinterpret_cast<HDC>(wp));return 0;
        case WM_SETTINGCHANGE:
        case WM_SYSCOLORCHANGE:
            appearance.colors();
            for(auto h:{window,settingsWindow,activityWindow})if(h)RedrawWindow(h,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN);
            return 0;
        case WM_SIZE:if(contentHeight)scrollTo(scrollY);return 0;
        case WM_VSCROLL:{
            SCROLLINFO info{sizeof(info),SIF_ALL};GetScrollInfo(hwnd,SB_VERT,&info);
            int position=scrollY;
            switch(LOWORD(wp)){
            case SB_LINEUP:position-=px(36);break;case SB_LINEDOWN:position+=px(36);break;
            case SB_PAGEUP:position-=static_cast<int>(info.nPage);break;case SB_PAGEDOWN:position+=static_cast<int>(info.nPage);break;
            case SB_THUMBTRACK:case SB_THUMBPOSITION:position=info.nTrackPos;break;
            case SB_TOP:position=0;break;case SB_BOTTOM:position=px(contentHeight);break;
            }
            scrollTo(position);return 0;}
        case WM_MOUSEWHEEL:
            wheelDelta+=GET_WHEEL_DELTA_WPARAM(wp);
            while(wheelDelta>=WHEEL_DELTA){scrollTo(scrollY-px(84));wheelDelta-=WHEEL_DELTA;}
            while(wheelDelta<=-WHEEL_DELTA){scrollTo(scrollY+px(84));wheelDelta+=WHEEL_DELTA;}
            return 0;
        case WM_NOTIFY:
            if(reinterpret_cast<NMHDR*>(lp)->code==NM_CUSTOMDRAW)return drawButton(reinterpret_cast<NMCUSTOMDRAW*>(lp));
            break;
        case WM_DRAWITEM:{
            auto item=reinterpret_cast<DRAWITEMSTRUCT*>(lp);
            if(item->CtlID<400 || item->CtlID>=432)break;
            bool active=static_cast<int>(item->CtlID)-400==activeMapping;
            FillRect(item->hDC,&item->rcItem,appearance.panelBrush);
            auto face=item->rcItem;face.right-=px(1);face.bottom-=px(3);
            auto shade=face;OffsetRect(&shade,0,px(3));appearance.round(item->hDC,shade,appearance.border,appearance.border,8);
            appearance.round(item->hDC,face,active?appearance.accent:appearance.tint,active?appearance.accent:appearance.border,8);
            InflateRect(&face,-px(5),0);
            appearance.text(item->hDC,value(item->hwndItem),face,appearance.key,
                active?(appearance.highContrast?GetSysColor(COLOR_HIGHLIGHTTEXT):RGB(255,255,255)):(appearance.highContrast?appearance.ink:appearance.accent),
                DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);return TRUE;}
        case WM_CTLCOLORBTN:case WM_CTLCOLORSTATIC:case WM_CTLCOLOREDIT:case WM_CTLCOLORLISTBOX:return controlColors(wp,lp);
        case WM_CLOSE:
            if(settingsWindow)ShowWindow(settingsWindow,SW_HIDE);
            closeActivity();ShowWindow(hwnd,SW_HIDE);return 0;
        case WM_COMMAND:
            if(HIWORD(wp)==CBN_SETFOCUS || HIWORD(wp)==BN_SETFOCUS){ensureVisible(reinterpret_cast<HWND>(lp));return 0;}
            if(LOWORD(wp)>=applyId && LOWORD(wp)<=activityId && HIWORD(wp)!=BN_CLICKED)return 0;
            if(LOWORD(wp)==profileId && HIWORD(wp)==CBN_SELCHANGE){
                auto next=static_cast<size_t>(SendMessageW(profileBox,CB_GETCURSEL,0,0));
                if(!discard(settingsWindow)){SendMessageW(profileBox,CB_SETCURSEL,selected,0);return 0;}
                sun::Profile newProfile;
                try{newProfile=sun::readProfile(profiles.at(next));}catch(...){SendMessageW(profileBox,CB_SETCURSEL,selected,0);throw;}
                profile=std::move(newProfile);selected=next;loadRows();saveSelected();start();return 0;
            }
            if(LOWORD(wp)>=200 && HIWORD(wp)!=CBN_EDITCHANGE && HIWORD(wp)!=CBN_SELCHANGE)return 0;
            command(LOWORD(wp),lp?GetAncestor(reinterpret_cast<HWND>(lp),GA_ROOT):hwnd);return 0;
        case WM_TIMER:updateStatus();return 0;
        case trayMessage:
            if(LOWORD(lp)==NIN_SELECT || LOWORD(lp)==NIN_KEYSELECT){show();return 0;}
            if(LOWORD(lp)==WM_CONTEXTMENU){
                POINT pt{};GetCursorPos(&pt);auto menu=CreatePopupMenu();
                AppendMenuW(menu,MF_STRING,1,L"Open Sun Keyboard Tools");
                AppendMenuW(menu,MF_STRING,settingsId,L"Settings...");AppendMenuW(menu,MF_STRING,activityId,L"Key monitor...");
                AppendMenuW(menu,MF_STRING,pauseId,paused?L"Resume":L"Pause");AppendMenuW(menu,MF_STRING,exitId,L"Exit");
                SetForegroundWindow(hwnd);auto id=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,pt.x,pt.y,0,hwnd,nullptr);DestroyMenu(menu);
                if(id==1)show();else if(id)command(id);return 0;
            }break;
        case WM_DESTROY:KillTimer(hwnd,1);backend.stop();Shell_NotifyIconW(NIM_DELETE,&tray);PostQuitMessage(0);return 0;
        }
    }catch(const std::exception& e){error(e,msg==WM_COMMAND && lp?GetAncestor(reinterpret_cast<HWND>(lp),GA_ROOT):hwnd);}
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
        dpi=GetDpiForSystem();appearance.init(dpi);
        WNDCLASSW cls{};cls.lpfnWndProc=proc;cls.hInstance=instance;cls.lpszClassName=windowClass;
        cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);cls.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(1));
        if(!RegisterClassW(&cls))throw std::runtime_error("Cannot register settings window: "+sun::utf8(sun::winError()));
        cls.lpfnWndProc=utilityProc;cls.lpszClassName=utilityClass;
        if(!RegisterClassW(&cls))throw std::runtime_error("Cannot register settings utility window: "+sun::utf8(sun::winError()));
        window=CreateWindowExW(WS_EX_CONTROLPARENT,windowClass,L"Sun Keyboard Tools",
            WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX|WS_VSCROLL|WS_CLIPCHILDREN,
            CW_USEDEFAULT,CW_USEDEFAULT,px(644),px(700),nullptr,nullptr,instance,nullptr);
        if(!window)throw std::runtime_error("Cannot create settings window: "+sun::utf8(sun::winError()));
        label(L"Sun Keyboard Tools",88,24,360,38,appearance.title,4);
        label(L"Your Sun keys, useful again.",90,65,400,24,appearance.normal,1|4);
        control(L"BUTTON",L"Settings...",WS_TABSTOP,480,34,120,34,settingsId,nullptr,4);
        profileTitle=label(sun::wide(profile.name),44,132,536,26,appearance.heading);
        for(int col=0;col<2;++col){
            label(L"SUN KEY",44+col*276,174,78,18,appearance.caption,1);
            label(L"WINDOWS SHORTCUT",130+col*276,174,168,18,appearance.caption,1);
        }
        loadRows();
        taskbarCreated=RegisterWindowMessageW(L"TaskbarCreated");addTray();SetTimer(window,1,500,nullptr);start();
        if(!background)ShowWindow(window,SW_SHOW);
        MSG msg{};
        while(GetMessageW(&msg,nullptr,0,0)>0){
            auto root=GetAncestor(msg.hwnd,GA_ROOT);
            if(root!=settingsWindow && root!=activityWindow)root=window;
            if(!IsDialogMessageW(root,&msg)){TranslateMessage(&msg);DispatchMessageW(&msg);}
            else if(root==window && msg.message==WM_KEYDOWN && msg.wParam==VK_TAB)ensureVisible(GetFocus());
        }
        appearance.release();CoUninitialize();return 0;
    }catch(const std::exception& e){error(e);CoUninitialize();return 1;}
}

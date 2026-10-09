#pragma once
#include <windows.h>
#include <string>
#include <initializer_list>

// GDI accents around standard Windows controls: keyboard navigation and the
// accessibility tree remain native. Respect the system high-contrast palette.
namespace sun::ui {
struct Appearance {
    UINT dpi{96};
    bool highContrast{};
    COLORREF canvas{}, panel{}, ink{}, muted{}, accent{}, accentHot{}, tint{}, border{}, shadow{}, green{}, amber{};
    HBRUSH canvasBrush{}, panelBrush{};
    HFONT normal{}, title{}, heading{}, caption{}, key{}, number{};
    int px(int n) const { return MulDiv(n,dpi,96); }
    HFONT font(int size,int weight=FW_NORMAL) const {
        return CreateFontW(-px(size),0,0,0,weight,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
            0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
    }
    void colors() {
        HIGHCONTRASTW hc{sizeof(hc)};
        SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(hc),&hc,0);
        highContrast=(hc.dwFlags&HCF_HIGHCONTRASTON)!=0;
        canvas=RGB(245,244,248); panel=RGB(255,255,255);
        ink=RGB(43,38,56); muted=RGB(104,99,117);
        accent=RGB(94,65,145); accentHot=RGB(76,48,126);
        tint=RGB(241,236,249); border=RGB(222,218,231); shadow=RGB(233,230,239);
        green=RGB(31,112,80); amber=RGB(146,91,17);
        if(highContrast) {
            canvas=panel=GetSysColor(COLOR_WINDOW); ink=muted=GetSysColor(COLOR_WINDOWTEXT);
            accent=accentHot=GetSysColor(COLOR_HIGHLIGHT); tint=panel;
            border=ink; shadow=panel; green=amber=ink;
        }
        if(canvasBrush)DeleteObject(canvasBrush);
        if(panelBrush)DeleteObject(panelBrush);
        canvasBrush=CreateSolidBrush(canvas); panelBrush=CreateSolidBrush(panel);
    }
    void init(UINT scale) {
        dpi=scale; colors(); normal=font(14); title=font(27,FW_SEMIBOLD);
        heading=font(16,FW_SEMIBOLD); caption=font(12); key=font(13,FW_SEMIBOLD); number=font(25,FW_SEMIBOLD);
    }
    void release() {
        for(auto f:{normal,title,heading,caption,key,number})if(f)DeleteObject(f);
        if(canvasBrush)DeleteObject(canvasBrush); if(panelBrush)DeleteObject(panelBrush);
    }
    void round(HDC dc,RECT r,COLORREF fill,COLORREF edge,int radius=10) const {
        auto brush=CreateSolidBrush(fill); auto pen=CreatePen(PS_SOLID,px(1),edge);
        auto oldBrush=SelectObject(dc,brush); auto oldPen=SelectObject(dc,pen);
        RoundRect(dc,r.left,r.top,r.right,r.bottom,px(radius),px(radius));
        SelectObject(dc,oldPen); SelectObject(dc,oldBrush); DeleteObject(pen); DeleteObject(brush);
    }
    void text(HDC dc,const std::wstring& s,RECT r,HFONT f,COLORREF color,UINT flags=DT_LEFT|DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS) const {
        auto old=SelectObject(dc,f); SetBkMode(dc,TRANSPARENT); SetTextColor(dc,color);
        DrawTextW(dc,s.c_str(),static_cast<int>(s.size()),&r,flags|DT_NOPREFIX); SelectObject(dc,old);
    }
};
}

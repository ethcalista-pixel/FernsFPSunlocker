#include <windows.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define TARGET  "Ferns.exe"
#define VERSION "1.2"

#define WM_TRAY   (WM_APP + 1)
#define IDM_SHOW   101
#define IDM_APPLY  102
#define IDM_EXIT   103

// Control IDs — 200s normal, 300s = dim labels, 400s = separators
#define ID_APPLY    200
#define ID_TOTRAY   201
#define ID_CHK_FPS  202
#define ID_CHK_PRI  203
#define ID_CHK_POW  204
#define ID_CHK_AFF  205
#define ID_CHK_AUTO 206
#define ID_EDIT_FPS 207
#define ID_STATUS   208
#define ID_SHADER_INSTALL 209
#define ID_SHADER_REMOVE  210
#define ID_LBL_FPS  300
#define ID_LBL_PERF 301
#define ID_LBL_OPT  302
#define ID_LBL_SHD  303
#define ID_SEP1     400
#define ID_SEP2     401

// Palette
#define C_BG    RGB(10,  10,  10)
#define C_FG    RGB(200, 200, 200)
#define C_DIM   RGB(65,  65,  65)
#define C_GREEN RGB(80,  200, 130)
#define C_IBKG  RGB(20,  20,  20)
#define C_SEP   RGB(38,  38,  38)

static HBRUSH g_br_bg, g_br_ib, g_br_sep, g_br_fg;
static HWND   g_hwnd;
static NOTIFYICONDATAA g_nid;
static BOOL   g_tray;
static UINT_PTR g_timer;
static HFONT  g_font;
static DWORD  g_last_pid;

// ---------------------------------------------------------------------------
// Frame cap core
// ---------------------------------------------------------------------------

static const float  cap60f=1.0f/60.0f, cap30f=1.0f/30.0f;
static const double cap60d=1.0/60.0,   cap30d=1.0/30.0;
static float  g_repf;
static double g_repd;

typedef struct { const void *needle; void *rep; int len; } Cap;
static Cap g_caps[4];

static void init_caps(int fps) {
    g_repf=1.0f/(float)fps; g_repd=1.0/(double)fps;
    g_caps[0]=(Cap){&cap60f,&g_repf,4}; g_caps[1]=(Cap){&cap60d,&g_repd,8};
    g_caps[2]=(Cap){&cap30f,&g_repf,4}; g_caps[3]=(Cap){&cap30d,&g_repd,8};
}

static DWORD find_pid(void) {
    HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if (snap==INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32 pe={sizeof(pe)}; DWORD pid=0;
    if (Process32First(snap,&pe))
        do { if (!_stricmp(pe.szExeFile,TARGET)){pid=pe.th32ProcessID;break;} }
        while (Process32Next(snap,&pe));
    CloseHandle(snap); return pid;
}

static int patch_fps(HANDLE h) {
    MEMORY_BASIC_INFORMATION mbi; uintptr_t addr=0; int total=0;
    while (VirtualQueryEx(h,(LPCVOID)addr,&mbi,sizeof(mbi))) {
        if (!mbi.RegionSize) break;
        DWORD p=mbi.Protect;
        if (mbi.State==MEM_COMMIT && (p==PAGE_READWRITE||p==PAGE_WRITECOPY||
            p==PAGE_EXECUTE_READWRITE||p==PAGE_EXECUTE_WRITECOPY)) {
            uint8_t *buf=malloc(mbi.RegionSize);
            if (buf) {
                SIZE_T rd=0;
                if (ReadProcessMemory(h,(LPCVOID)addr,buf,mbi.RegionSize,&rd)&&rd>0)
                    for (int c=0;c<4;c++) { int n=g_caps[c].len;
                        for (SIZE_T i=0;i+n<=rd;i++)
                            if (!memcmp(buf+i,g_caps[c].needle,n)) {
                                SIZE_T wr=0;
                                WriteProcessMemory(h,(LPVOID)(addr+i),g_caps[c].rep,n,&wr);
                                if (wr) total++; } }
                free(buf); } }
        addr+=mbi.RegionSize; }
    return total;
}

static BOOL disable_ecoqos(HANDLE h) {
    typedef BOOL(WINAPI*Fn)(HANDLE,int,LPVOID,DWORD);
    HMODULE k=GetModuleHandleA("kernel32.dll");
    Fn fn=k?(Fn)GetProcAddress(k,"SetProcessInformation"):NULL;
    if (!fn) return FALSE;
    struct{ULONG v,ctrl,state;} pts={1,1,0};
    return fn(h,4,&pts,sizeof(pts));
}

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------

static char g_sbuf[1024]; static int g_spos;
static void s_reset(void){g_spos=0;g_sbuf[0]='\0';}
static void s_add(const char*fmt,...){va_list a;va_start(a,fmt);g_spos+=vsnprintf(g_sbuf+g_spos,sizeof(g_sbuf)-g_spos,fmt,a);va_end(a);}
static void s_flush(void){SetDlgItemTextA(g_hwnd,ID_STATUS,g_sbuf);}

// ---------------------------------------------------------------------------
// Apply
// ---------------------------------------------------------------------------

static void do_apply(void) {
    char buf[16]; GetDlgItemTextA(g_hwnd,ID_EDIT_FPS,buf,sizeof(buf));
    int fps=atoi(buf); if(fps<1||fps>99999) fps=9999;
    init_caps(fps);

    DWORD pid=find_pid(); s_reset();
    if (!pid) { s_add("ferns.exe not found.\nopen the game first."); s_flush(); return; }
    g_last_pid=pid;

    BOOL do_fps  = IsDlgButtonChecked(g_hwnd,ID_CHK_FPS) ==BST_CHECKED;
    BOOL do_pri  = IsDlgButtonChecked(g_hwnd,ID_CHK_PRI) ==BST_CHECKED;
    BOOL do_pow  = IsDlgButtonChecked(g_hwnd,ID_CHK_POW) ==BST_CHECKED;
    BOOL do_aff  = IsDlgButtonChecked(g_hwnd,ID_CHK_AFF) ==BST_CHECKED;

    DWORD acc=PROCESS_VM_READ|PROCESS_VM_WRITE|PROCESS_VM_OPERATION|PROCESS_QUERY_INFORMATION;
    if (do_pri||do_aff||do_pow) acc|=PROCESS_SET_INFORMATION;

    HANDLE h=OpenProcess(acc,FALSE,pid);
    if (!h) { s_add("access denied (%lu).\nrun as administrator.",GetLastError()); s_flush(); return; }

    s_add("pid: %lu\n\n",pid);

    if (do_fps) {
        int n=patch_fps(h);
        if (n) s_add("fps cap removed  [%d patch%s]\n",n,n==1?"":"es");
        else   s_add("fps: nothing found\n");
    }
    if (do_pri) {
        if (SetPriorityClass(h,HIGH_PRIORITY_CLASS)) s_add("priority  ->  high\n");
        else s_add("priority: failed\n");
    }
    if (do_pow) {
        if (disable_ecoqos(h)) s_add("power throttling  ->  off\n");
        else s_add("power throttling: unsupported\n");
    }
    if (do_aff) {
        DWORD_PTR pm=0,sm=0;
        if (GetProcessAffinityMask(h,&pm,&sm)) {
            int tot=0; for(DWORD_PTR m=sm;m;m>>=1) tot+=(int)(m&1);
            int p=tot>2?tot/2:tot;
            DWORD_PTR nm=((DWORD_PTR)1<<p)-1;
            if (SetProcessAffinityMask(h,nm&sm)) s_add("affinity  ->  %d p-core%s\n",p,p==1?"":"s");
            else s_add("affinity: failed\n");
        }
    }

    CloseHandle(h); s_flush();
    if (g_tray) { strcpy(g_nid.szTip,"Ferns Unlocker \x97 active"); Shell_NotifyIconA(NIM_MODIFY,&g_nid); }
}

// ---------------------------------------------------------------------------
// Shader (SSR + SSAO post-process)
//
// The compiled DXGI proxy DLL (dxgi.dll, built from shader/proxy.c) is embedded
// in this exe as an RCDATA resource. "install" writes it into the Ferns client
// folder beside Ferns.exe, where it loads as a proxy in front of the real
// system dxgi.dll and applies the post-process. "remove" deletes it.
// ---------------------------------------------------------------------------

static BOOL get_shader_path(char *out, size_t n) {
    char base[MAX_PATH];
    if (SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, base) != S_OK) return FALSE;
    _snprintf(out, n, "%s\\Ferns\\Client\\dxgi.dll", base);
    out[n-1] = '\0';
    return TRUE;
}

static void install_shader(void) {
    char path[MAX_PATH];
    s_reset();
    if (!get_shader_path(path, sizeof(path))) {
        s_add("could not locate the ferns client folder."); s_flush(); return;
    }

    HRSRC res = FindResourceA(NULL, "SHADER_DLL", RT_RCDATA);
    if (!res) { s_add("shader missing from this build."); s_flush(); return; }
    HGLOBAL hg = LoadResource(NULL, res);
    DWORD   sz = SizeofResource(NULL, res);
    void   *data = hg ? LockResource(hg) : NULL;
    if (!data || !sz) { s_add("shader resource load failed."); s_flush(); return; }

    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        s_add("install failed (%lu).\nclose the game first.", GetLastError());
        s_flush(); return;
    }
    DWORD wr = 0; BOOL ok = WriteFile(f, data, sz, &wr, NULL);
    CloseHandle(f);

    if (ok && wr == sz) s_add("shader installed  [%lu KB]\nrelaunch the game.", sz/1024);
    else                s_add("shader write incomplete.");
    s_flush();
}

static void uninstall_shader(void) {
    char path[MAX_PATH];
    s_reset();
    if (!get_shader_path(path, sizeof(path))) {
        s_add("could not locate the ferns client folder."); s_flush(); return;
    }
    if (DeleteFileA(path)) { s_add("shader removed.\nrelaunch the game."); }
    else {
        DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND) s_add("shader not installed.");
        else s_add("remove failed (%lu).\nclose the game first.", e);
    }
    s_flush();
}

// ---------------------------------------------------------------------------
// Tray
// ---------------------------------------------------------------------------

static void tray_add(void) {
    memset(&g_nid,0,sizeof(g_nid));
    g_nid.cbSize=sizeof(g_nid); g_nid.hWnd=g_hwnd; g_nid.uID=1;
    g_nid.uFlags=NIF_ICON|NIF_MESSAGE|NIF_TIP;
    g_nid.uCallbackMessage=WM_TRAY;
    g_nid.hIcon=LoadIconA(NULL,IDI_APPLICATION);
    strcpy(g_nid.szTip,"Ferns FPS Unlocker");
    Shell_NotifyIconA(NIM_ADD,&g_nid); g_tray=TRUE;
}
static void tray_remove(void){ Shell_NotifyIconA(NIM_DELETE,&g_nid); g_tray=FALSE; }
static void tray_menu(void) {
    POINT pt; GetCursorPos(&pt);
    HMENU m=CreatePopupMenu();
    AppendMenuA(m,MF_STRING,IDM_SHOW, "show");
    AppendMenuA(m,MF_STRING,IDM_APPLY,"apply");
    AppendMenuA(m,MF_SEPARATOR,0,NULL);
    AppendMenuA(m,MF_STRING,IDM_EXIT, "exit");
    SetForegroundWindow(g_hwnd);
    TrackPopupMenu(m,TPM_RIGHTBUTTON,pt.x,pt.y,0,g_hwnd,NULL);
    DestroyMenu(m);
}

static void CALLBACK on_timer(HWND hw,UINT msg,UINT_PTR id,DWORD t) {
    (void)hw;(void)msg;(void)id;(void)t;
    DWORD pid=find_pid();
    if (pid&&pid!=g_last_pid) do_apply();
    if (!pid) g_last_pid=0;
}

// ---------------------------------------------------------------------------
// Owner-drawn button
// ---------------------------------------------------------------------------

static void draw_button(LPDRAWITEMSTRUCT dis) {
    HDC dc=dis->hDC; RECT r=dis->rcItem;
    BOOL pressed=(dis->itemState&ODS_SELECTED)!=0;
    FillRect(dc,&r,pressed?g_br_fg:g_br_bg);
    HPEN pen=CreatePen(PS_SOLID,1,pressed?C_BG:C_FG);
    HGDIOBJ old=SelectObject(dc,pen);
    SelectObject(dc,GetStockObject(NULL_BRUSH));
    Rectangle(dc,r.left,r.top,r.right,r.bottom);
    SelectObject(dc,old); DeleteObject(pen);
    SetBkMode(dc,TRANSPARENT);
    SetTextColor(dc,pressed?C_BG:C_FG);
    SelectObject(dc,g_font);
    char txt[64]; GetWindowTextA(dis->hwndItem,txt,sizeof(txt));
    DrawTextA(dc,txt,-1,&r,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
}

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------

static LRESULT CALLBACK WndProc(HWND hw,UINT msg,WPARAM wp,LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND: {
        RECT r; GetClientRect(hw,&r); FillRect((HDC)wp,&r,g_br_bg); return 1;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc=(HDC)wp; HWND ctrl=(HWND)lp;
        int id=GetDlgCtrlID(ctrl);
        SetBkMode(dc,TRANSPARENT);
        if (id>=400)                            { return (LRESULT)g_br_sep; }
        if (id>=300)                            { SetTextColor(dc,C_DIM); return (LRESULT)g_br_bg; }
        if (id==ID_STATUS)                      { SetTextColor(dc,C_GREEN); return (LRESULT)g_br_bg; }
        SetTextColor(dc,C_FG); return (LRESULT)g_br_bg;
    }
    case WM_CTLCOLOREDIT: {
        SetBkColor((HDC)wp,C_IBKG); SetTextColor((HDC)wp,C_FG);
        return (LRESULT)g_br_ib;
    }
    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT dis=(LPDRAWITEMSTRUCT)lp;
        if (dis->CtlType==ODT_BUTTON) draw_button(dis);
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_APPLY:   do_apply(); break;
        case ID_SHADER_INSTALL: install_shader(); break;
        case ID_SHADER_REMOVE:  uninstall_shader(); break;
        case ID_TOTRAY:  tray_add(); ShowWindow(hw,SW_HIDE); break;
        case IDM_SHOW:   ShowWindow(hw,SW_RESTORE); SetForegroundWindow(hw); break;
        case IDM_APPLY:  do_apply(); break;
        case IDM_EXIT:   DestroyWindow(hw); break;
        case ID_CHK_AUTO:
            if (IsDlgButtonChecked(hw,ID_CHK_AUTO)==BST_CHECKED) {
                if (!g_timer) g_timer=SetTimer(hw,1,3000,on_timer);
            } else {
                if (g_timer) { KillTimer(hw,g_timer); g_timer=0; }
            }
            break;
        }
        break;
    case WM_TRAY:
        if      (lp==WM_RBUTTONUP)      tray_menu();
        else if (lp==WM_LBUTTONDBLCLK) { ShowWindow(hw,SW_RESTORE); SetForegroundWindow(hw); }
        break;
    case WM_DESTROY:
        if (g_tray)  tray_remove();
        if (g_timer) KillTimer(hw,g_timer);
        PostQuitMessage(0); break;
    default:
        return DefWindowProcA(hw,msg,wp,lp);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// UI helpers
// ---------------------------------------------------------------------------

static BOOL CALLBACK set_font(HWND hw,LPARAM lp){ SendMessage(hw,WM_SETFONT,(WPARAM)(HFONT)lp,TRUE); return TRUE; }

static void mk_label(HWND p,const char*t,int id,int x,int y,int w,int h) {
    CreateWindowA("STATIC",t,WS_CHILD|WS_VISIBLE|SS_NOPREFIX,x,y,w,h,p,(HMENU)(intptr_t)id,NULL,NULL);
}
static void mk_check(HWND p,const char*t,int id,int x,int y,int w,BOOL on) {
    HWND h=CreateWindowA("BUTTON",t,WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,x,y,w,18,p,(HMENU)(intptr_t)id,NULL,NULL);
    if (on) SendMessage(h,BM_SETCHECK,BST_CHECKED,0);
}
static void mk_btn(HWND p,const char*t,int id,int x,int y,int w,int h) {
    CreateWindowA("BUTTON",t,WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,x,y,w,h,p,(HMENU)(intptr_t)id,NULL,NULL);
}
static void mk_sep(HWND p,int id,int x,int y,int w) {
    CreateWindowA("STATIC","",WS_CHILD|WS_VISIBLE,x,y,w,1,p,(HMENU)(intptr_t)id,NULL,NULL);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int WINAPI WinMain(HINSTANCE hi,HINSTANCE hp,LPSTR cmd,int show) {
    (void)hp; (void)cmd;

    g_br_bg  = CreateSolidBrush(C_BG);
    g_br_ib  = CreateSolidBrush(C_IBKG);
    g_br_sep = CreateSolidBrush(C_SEP);
    g_br_fg  = CreateSolidBrush(C_FG);

    g_font = CreateFontA(-13,0,0,0,FW_NORMAL,0,0,0,
        DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_DONTCARE,"Consolas");

    WNDCLASSEXA wc={sizeof(wc)};
    wc.lpfnWndProc  =WndProc;
    wc.hInstance    =hi;
    wc.hCursor      =LoadCursor(NULL,IDC_ARROW);
    wc.hbrBackground=g_br_bg;
    wc.lpszClassName="FernsUnlocker";
    RegisterClassExA(&wc);

    RECT rc={0,0,358,404};
    AdjustWindowRect(&rc,WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,FALSE);

    g_hwnd=CreateWindowExA(0,"FernsUnlocker",
        "ferns fps unlocker  v" VERSION,
        WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,
        CW_USEDEFAULT,CW_USEDEFAULT,
        rc.right-rc.left,rc.bottom-rc.top,
        NULL,NULL,hi,NULL);

    // ── Controls ─────────────────────────────────────────────────
    HWND hw=g_hwnd;
    int x=18, y=14, w=322;

    mk_sep  (hw,ID_SEP1,    x,  y,  w);       y+=10;
    mk_label(hw,"fps",      ID_LBL_FPS, x, y, w, 14); y+=20;
    mk_check(hw,"remove fps cap",ID_CHK_FPS,x,y,152,TRUE);
    mk_label(hw,"target:",  0, x+160, y+1, 50, 14);
    HWND ed=CreateWindowA("EDIT","9999",
        WS_CHILD|WS_VISIBLE|WS_BORDER|ES_NUMBER,
        x+214,y,68,18,hw,(HMENU)ID_EDIT_FPS,NULL,NULL);
    SendMessage(ed,EM_SETLIMITTEXT,5,0);
    y+=28;

    mk_sep  (hw,0,          x,  y,  w);       y+=10;
    mk_label(hw,"performance",ID_LBL_PERF,x,y,w,14); y+=20;
    mk_check(hw,"boost process priority",   ID_CHK_PRI, x, y, w, TRUE);  y+=22;
    mk_check(hw,"disable power throttling", ID_CHK_POW, x, y, w, TRUE);  y+=22;
    mk_check(hw,"pin to p-cores  (intel 12th gen+)",ID_CHK_AFF,x,y,w,FALSE); y+=28;

    mk_sep  (hw,0,          x,  y,  w);       y+=10;
    mk_label(hw,"options",  ID_LBL_OPT, x, y, w, 14); y+=20;
    mk_check(hw,"auto-reapply on rejoin",   ID_CHK_AUTO,x, y, w, FALSE); y+=28;

    mk_sep  (hw,0,          x,  y,  w);       y+=10;
    mk_label(hw,"shader  (SSR + SSAO, experimental)",ID_LBL_SHD,x,y,w,14); y+=20;
    mk_btn  (hw,"install shader",ID_SHADER_INSTALL,x,     y,156,24);
    mk_btn  (hw,"remove shader", ID_SHADER_REMOVE, x+166, y,156,24);   y+=32;

    mk_sep  (hw,ID_SEP2,    x,  y,  w);       y+=12;

    CreateWindowA("STATIC","idle.  open the game and press apply.",
        WS_CHILD|WS_VISIBLE|SS_NOPREFIX|SS_LEFT,
        x,y,w,60,hw,(HMENU)ID_STATUS,NULL,NULL);
    y+=68;

    mk_btn(hw,"apply",          ID_APPLY, x,     y, 96, 26);
    mk_btn(hw,"minimize to tray",ID_TOTRAY,x+104,y,140,26);

    EnumChildWindows(g_hwnd,set_font,(LPARAM)g_font);

    ShowWindow(g_hwnd,show);
    UpdateWindow(g_hwnd);

    MSG m;
    while (GetMessageA(&m,NULL,0,0)) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }

    DeleteObject(g_font);
    DeleteObject(g_br_bg); DeleteObject(g_br_ib);
    DeleteObject(g_br_sep); DeleteObject(g_br_fg);
    return (int)m.wParam;
}

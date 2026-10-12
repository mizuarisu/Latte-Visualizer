/*
 * LiteVis 2 - tiny audio visualizer wallpaper for Windows.
 * Pure Win32 + WASAPI loopback + software renderer. Tray icon -> settings.
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00
#define _WIN32_IE 0x0A00
#define NTDDI_VERSION 0x0A000000
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <tlhelp32.h>
#include <propidl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commctrl.h>
#include <commdlg.h>
#include <wchar.h>
#include <stdio.h>
#include <stdarg.h>
#include "render.h"
/* Hardcode WASAPI GUIDs to satisfy the MSVC linker in pure C mode */
const GUID CLSID_MMDeviceEnumerator = { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
const GUID IID_IMMDeviceEnumerator = { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
const GUID IID_IAudioClient = { 0x1CB9AD4C, 0xDBFA, 0x4C32, { 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2 } };
const GUID IID_IAudioCaptureClient = { 0xC8ADBD64, 0xE71E, 0x48A0, { 0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17 } };
const GUID IID_IAudioSessionManager2 = { 0x77AA99A0, 0x1BD6, 0x484F, { 0x8B, 0xC7, 0x2C, 0x65, 0x4C, 0x9A, 0x9B, 0x6F } };
const GUID IID_IAudioSessionControl2 = { 0xBFB7FF88, 0x7239, 0x4FC9, { 0x8F, 0xA2, 0x07, 0xC9, 0x50, 0xBE, 0x9C, 0x6D } };

/* ---------------- GDI+ flat API (declared by hand, plain C) ---------------- */
typedef struct { UINT32 ver; void *cb; BOOL noThread, noCodecs; } GpInput;
extern int  WINAPI GdiplusStartup(ULONG_PTR *tok, const GpInput *in, void *out);
extern void WINAPI GdiplusShutdown(ULONG_PTR tok);
extern int  WINAPI GdipCreateBitmapFromFile(const WCHAR *f, void **bmp);
extern int  WINAPI GdipGetImageWidth(void *img, UINT *w);
extern int  WINAPI GdipGetImageHeight(void *img, UINT *h);
extern int  WINAPI GdipCreateFromHDC(HDC dc, void **g);
extern int  WINAPI GdipSetInterpolationMode(void *g, int m);
extern int  WINAPI GdipDrawImageRectRectI(void *g, void *img, int dx, int dy, int dw, int dh,
                                          int sx, int sy, int sw, int sh, int unit,
                                          void *attr, void *cb, void *cbdata);
extern int  WINAPI GdipDeleteGraphics(void *g);
extern int  WINAPI GdipDisposeImage(void *img);

/* ---------------- globals ---------------- */
#define WM_TRAY (WM_USER + 1)
#define RUNKEY  L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define APPKEY  L"Software\\LiteVis"

static HINSTANCE hInst;
static HWND hCtl, hWall, hSet;
static Settings S;
static int sw, sh, dpi = 96, stripH, stripTop, paused, busy, tickN;
static HDC bgDC, frDC;
static HBITMAP bgBmp, frBmp;
static uint32_t *bgBits, *frBits;
static Canvas cv;
static NOTIFYICONDATAW nid;
static UINT wmTaskbar;
static HDC fullDC;                        /* full-screen composite, only used by the UpdateLayeredWindow methods */
static HBITMAP fullBmp;
static uint32_t *fullBits;
static int g_ulw;
static float demoLeft, demoT;            /* "Test bars" animation (works without any audio) */
static HFONT uiFont;

#define SC(x) MulDiv((x), dpi, 96)

/* optional diagnostics: run  litevis.exe --log  -> %TEMP%\litevis.log */
static int g_log;
static void lv_log(const char *fmt, ...)
{
    char path[MAX_PATH + 16], line[512];
    va_list ap;
    if (!g_log) return;
    GetTempPathA(MAX_PATH, path);
    lstrcatA(path, "litevis.log");
    FILE *f = fopen(path, "a");
    if (!f) return;
    va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    fprintf(f, "%s\n", line);
    fclose(f);
}

/* ---------------- audio ---------------- */
static IMMDeviceEnumerator *en;
static IAudioClient *ac;
static IAudioCaptureClient *cc;
static int channels = 2, bits = 32, audioOK;
static WCHAR devId[256];

/* ---- per-app capture (process loopback, Windows 10 version 2004+) ---- */
#define MAXAPPS 16
#define MAXCAPS 16
typedef struct { IAudioClient *ac; IAudioCaptureClient *cc; HANDLE ev; DWORD pid; int ok; } Cap;
static Cap caps[MAXCAPS];
static WCHAR appNames[MAXAPPS][64];
static int nApps;
static int wantApps(void) { return S.appOnly && nApps > 0; }

typedef struct { int ActivationType; DWORD TargetProcessId; int ProcessLoopbackMode; } ActParams;

typedef struct IAsyncOp IAsyncOp;
typedef struct {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IAsyncOp *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(IAsyncOp *);
    ULONG (STDMETHODCALLTYPE *Release)(IAsyncOp *);
    HRESULT (STDMETHODCALLTYPE *GetActivateResult)(IAsyncOp *, HRESULT *, IUnknown **);
} AsyncOpVtbl;
struct IAsyncOp { const AsyncOpVtbl *lpVtbl; };

typedef struct Handler Handler;
typedef struct {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(Handler *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(Handler *);
    ULONG (STDMETHODCALLTYPE *Release)(Handler *);
    HRESULT (STDMETHODCALLTYPE *ActivateCompleted)(Handler *, IAsyncOp *);
} HandlerVtbl;
struct Handler { const HandlerVtbl *lpVtbl; LONG ref; HANDLE ev; };

static const GUID IID_ActHandler = {0x41D949AB, 0x9862, 0x444A, {0x80, 0xF6, 0xC2, 0x61, 0x33, 0x4D, 0xA5, 0xEB}};
static const GUID IID_AgileObj   = {0x94EA2B94, 0xE9CC, 0x49E0, {0xC0, 0xFF, 0xEE, 0x64, 0xCA, 0x8F, 0x5B, 0x90}};

static HRESULT STDMETHODCALLTYPE h_qi(Handler *t, REFIID r, void **o)
{
    if (IsEqualGUID(r, &IID_IUnknown) || IsEqualGUID(r, &IID_ActHandler) || IsEqualGUID(r, &IID_AgileObj)) {
        *o = t; InterlockedIncrement(&t->ref); return S_OK;
    }
    *o = NULL; return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE h_addref(Handler *t) { return (ULONG)InterlockedIncrement(&t->ref); }
static ULONG STDMETHODCALLTYPE h_release(Handler *t)
{
    LONG n = InterlockedDecrement(&t->ref);
    if (n == 0) { if (t->ev) CloseHandle(t->ev); free(t); }
    return (ULONG)n;
}
static HRESULT STDMETHODCALLTYPE h_done(Handler *t, IAsyncOp *op) { (void)op; SetEvent(t->ev); return S_OK; }
static const HandlerVtbl hvt = {h_qi, h_addref, h_release, h_done};

typedef HRESULT (WINAPI *PFN_Activate)(LPCWSTR, REFIID, PROPVARIANT *, Handler *, IAsyncOp **);
static PFN_Activate pActivate;

static void cap_close(Cap *c)
{
    if (c->ac) IAudioClient_Stop(c->ac);
    if (c->cc) IAudioCaptureClient_Release(c->cc);
    if (c->ac) IAudioClient_Release(c->ac);
    if (c->ev) CloseHandle(c->ev);
    memset(c, 0, sizeof *c);
}

static int cap_open(Cap *c, DWORD pid)
{
    ActParams ap = {1, pid, 0};                     /* PROCESS_LOOPBACK, include process tree */
    PROPVARIANT pv;
    IAsyncOp *op = NULL;
    IUnknown *u = NULL;
    IAudioClient *acl = NULL;
    HRESULT hr, ar = E_FAIL;
    WAVEFORMATEX wf = {WAVE_FORMAT_IEEE_FLOAT, 2, 48000, 48000 * 8, 8, 32, 0};

    if (!pActivate) {
        HMODULE m = LoadLibraryW(L"mmdevapi.dll");
        if (m) pActivate = (PFN_Activate)(void *)GetProcAddress(m, "ActivateAudioInterfaceAsync");
        if (!pActivate) { lv_log("cap_open: ActivateAudioInterfaceAsync missing"); return 0; }
    }
    memset(&pv, 0, sizeof pv);
    pv.vt = VT_BLOB; pv.blob.cbSize = sizeof ap; pv.blob.pBlobData = (BYTE *)&ap;

    Handler *h = (Handler *)calloc(1, sizeof *h);
    h->lpVtbl = &hvt; h->ref = 1; h->ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    hr = pActivate(L"VAD\\Process_Loopback", &IID_IAudioClient, &pv, h, &op);
    if (FAILED(hr)) { lv_log("cap_open pid=%lu: activate hr=%08lx", pid, (unsigned long)hr); h->lpVtbl->Release(h); return 0; }
    WaitForSingleObject(h->ev, 3000);
    if (op) { op->lpVtbl->GetActivateResult(op, &ar, &u); op->lpVtbl->Release(op); }
    h->lpVtbl->Release(h);
    if (FAILED(ar) || !u) { lv_log("cap_open pid=%lu: result hr=%08lx", pid, (unsigned long)ar); return 0; }
    hr = IUnknown_QueryInterface(u, &IID_IAudioClient, (void **)&acl);
    IUnknown_Release(u);
    if (FAILED(hr)) return 0;

    c->ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    hr = IAudioClient_Initialize(acl, AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                 200000, 0, &wf, NULL);
    if (SUCCEEDED(hr)) hr = IAudioClient_SetEventHandle(acl, c->ev);
    if (SUCCEEDED(hr)) hr = IAudioClient_GetService(acl, &IID_IAudioCaptureClient, (void **)&c->cc);
    if (SUCCEEDED(hr)) hr = IAudioClient_Start(acl);
    if (FAILED(hr)) {
        lv_log("cap_open pid=%lu: init hr=%08lx", pid, (unsigned long)hr);
        c->ac = acl; cap_close(c);
        return 0;
    }
    c->ac = acl; c->pid = pid; c->ok = 1;
    g_srate = 48000;
    lv_log("cap_open pid=%lu: ok", pid);
    return 1;
}

/* root process ids of every configured app (Chrome/Spotify/etc. play audio from child processes) */
static int list_roots(DWORD *out, int max)
{
    static PROCESSENTRY32W pe[1024];
    int np = 0, n = 0;
    HANDLE sn = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (sn == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W e; e.dwSize = sizeof e;
    if (Process32FirstW(sn, &e)) do { if (np < 1024) pe[np++] = e; } while (Process32NextW(sn, &e));
    CloseHandle(sn);
    for (int a = 0; a < nApps; a++)
        for (int i = 0; i < np; i++) {
            if (_wcsicmp(pe[i].szExeFile, appNames[a])) continue;
            int root = 1, dup = 0;
            for (int j = 0; j < np; j++)
                if (!_wcsicmp(pe[j].szExeFile, appNames[a]) && pe[j].th32ProcessID == pe[i].th32ParentProcessID) root = 0;
            for (int k = 0; k < n; k++) if (out[k] == pe[i].th32ProcessID) dup = 1;
            if (root && !dup && n < max) out[n++] = pe[i].th32ProcessID;
        }
    return n;
}

static void apps_sync(void)
{
    DWORD want[MAXCAPS];
    int nw = list_roots(want, MAXCAPS), i, j;
    for (i = 0; i < MAXCAPS; i++) {
        int keep = 0;
        if (!caps[i].ok) continue;
        for (j = 0; j < nw; j++) if (want[j] == caps[i].pid) keep = 1;
        if (!keep) cap_close(&caps[i]);
    }
    for (j = 0; j < nw; j++) {
        int have = 0;
        for (i = 0; i < MAXCAPS; i++) if (caps[i].ok && caps[i].pid == want[j]) have = 1;
        if (have) continue;
        for (i = 0; i < MAXCAPS; i++) if (!caps[i].ok) { cap_open(&caps[i], want[j]); break; }
    }
}

/* ---- whole-system capture ---- */
static void audio_close(void)
{
    audioOK = 0;
    if (ac) { IAudioClient_Stop(ac); }
    if (cc) { IAudioCaptureClient_Release(cc); cc = NULL; }
    if (ac) { IAudioClient_Release(ac); ac = NULL; }
}

static int audio_open(void)
{
    IMMDevice *dev = NULL;
    WAVEFORMATEX *wf = NULL;
    LPWSTR id = NULL;
    audio_close();
    if (!en && FAILED(CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                       &IID_IMMDeviceEnumerator, (void **)&en)))
        return 0;
    if (FAILED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev))) return 0;
    if (SUCCEEDED(IMMDevice_GetId(dev, &id))) {
        lstrcpynW(devId, id, 256);
        CoTaskMemFree(id);
    }
    if (FAILED(IMMDevice_Activate(dev, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&ac))) goto fail;
    if (FAILED(IAudioClient_GetMixFormat(ac, &wf))) goto fail;
    channels = wf->nChannels; bits = wf->wBitsPerSample; g_srate = wf->nSamplesPerSec;
    if (FAILED(IAudioClient_Initialize(ac, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                       2000000, 0, wf, NULL))) goto fail;
    if (FAILED(IAudioClient_GetService(ac, &IID_IAudioCaptureClient, (void **)&cc))) goto fail;
    CoTaskMemFree(wf);
    IMMDevice_Release(dev);
    audioOK = SUCCEEDED(IAudioClient_Start(ac));
    lv_log("audio open: ok=%d channels=%d bits=%d rate=%d", audioOK, channels, bits, g_srate);
    return audioOK;
fail:
    if (wf) CoTaskMemFree(wf);
    IMMDevice_Release(dev);
    audio_close();
    return 0;
}

/* follow the default playback device (headphones <-> speakers) */
static void audio_check_device(void)
{
    IMMDevice *dev = NULL;
    LPWSTR id = NULL;
    int change = !audioOK;
    if (en && SUCCEEDED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev))) {
        if (SUCCEEDED(IMMDevice_GetId(dev, &id))) {
            if (lstrcmpW(id, devId)) change = 1;
            CoTaskMemFree(id);
        }
        IMMDevice_Release(dev);
    }
    if (change) audio_open();
}

/* choose between whole-system capture and the selected apps */
static void audio_apply_mode(void)
{
    if (wantApps()) {
        if (ac) audio_close();
        apps_sync();
    } else {
        for (int i = 0; i < MAXCAPS; i++) if (caps[i].ok) cap_close(&caps[i]);
        if (!audioOK) audio_open();
    }
}

/* ---- sample pump: sums every active capture into the analyzer ring ---- */
#define ACCMAX 16384
static float acc[ACCMAX];
static int accN;

static int pump_one(IAudioCaptureClient *c, int ch, int bt)
{
    UINT32 n;
    int idx = 0;
    for (;;) {
        if (FAILED(IAudioCaptureClient_GetNextPacketSize(c, &n))) return 0;
        if (!n) break;
        BYTE *d; UINT32 fr; DWORD fl;
        if (FAILED(IAudioCaptureClient_GetBuffer(c, &d, &fr, &fl, NULL, NULL))) return 0;
        for (UINT32 i = 0; i < fr; i++) {
            float s = 0;
            if (!(fl & AUDCLNT_BUFFERFLAGS_SILENT)) {
                if (bt == 32) {
                    float *f = (float *)d + (size_t)i * ch;
                    for (int k = 0; k < ch; k++) s += f[k];
                } else if (bt == 16) {
                    short *f = (short *)d + (size_t)i * ch;
                    for (int k = 0; k < ch; k++) s += f[k] / 32768.0f;
                }
                s /= ch;
            }
            if (idx < ACCMAX) acc[idx] += s;
            idx++;
        }
        IAudioCaptureClient_ReleaseBuffer(c, fr);
    }
    if (idx > ACCMAX) idx = ACCMAX;
    if (idx > accN) accN = idx;
    return 1;
}

static void audio_pump(float dt)
{
    accN = 0;
    if (audioOK && !pump_one(cc, channels, bits)) audioOK = 0;
    for (int i = 0; i < MAXCAPS; i++)
        if (caps[i].ok && !pump_one(caps[i].cc, 2, 32)) cap_close(&caps[i]);
    if (accN > 0) {
        for (int i = 0; i < accN; i++) { g_ring[g_ringPos] = acc[i]; g_ringPos = (g_ringPos + 1) & (FFT_N - 1); acc[i] = 0; }
    } else {                                  /* loopback sends nothing during silence: feed zeros so bars fall */
        int z = (int)(dt * g_srate);
        if (z > FFT_N) z = FFT_N;
        for (int i = 0; i < z; i++) { g_ring[g_ringPos] = 0; g_ringPos = (g_ringPos + 1) & (FFT_N - 1); }
    }
}

/* ---- helpers for the "Add app" menu ---- */
static void exe_name_of_pid(DWORD pid, WCHAR *out, int cch)
{
    WCHAR path[MAX_PATH]; DWORD sz = MAX_PATH;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    out[0] = 0;
    if (!p) return;
    if (QueryFullProcessImageNameW(p, 0, path, &sz)) {
        WCHAR *b = wcsrchr(path, L'\\');
        lstrcpynW(out, b ? b + 1 : path, cch);
    }
    CloseHandle(p);
}

static int app_known(const WCHAR *name)
{
    for (int i = 0; i < nApps; i++) if (!_wcsicmp(appNames[i], name)) return 1;
    return 0;
}

/* apps that currently have an audio session on the default output device */
static int list_audio_apps(WCHAR out[][64], int max)
{
    IMMDevice *dev = NULL;
    IAudioSessionManager2 *mgr = NULL;
    IAudioSessionEnumerator *se = NULL;
    int n = 0, cnt = 0;
    if (!en && FAILED(CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                       &IID_IMMDeviceEnumerator, (void **)&en))) return 0;
    if (FAILED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev))) return 0;
    if (SUCCEEDED(IMMDevice_Activate(dev, &IID_IAudioSessionManager2, CLSCTX_ALL, NULL, (void **)&mgr)) &&
        SUCCEEDED(IAudioSessionManager2_GetSessionEnumerator(mgr, &se))) {
        IAudioSessionEnumerator_GetCount(se, &cnt);
        for (int i = 0; i < cnt && n < max; i++) {
            IAudioSessionControl *ctl = NULL;
            IAudioSessionControl2 *c2 = NULL;
            DWORD pid = 0;
            WCHAR nm[64];
            if (FAILED(IAudioSessionEnumerator_GetSession(se, i, &ctl))) continue;
            if (SUCCEEDED(IAudioSessionControl_QueryInterface(ctl, &IID_IAudioSessionControl2, (void **)&c2))) {
                IAudioSessionControl2_GetProcessId(c2, &pid);
                IAudioSessionControl2_Release(c2);
            }
            IAudioSessionControl_Release(ctl);
            if (!pid) continue;
            exe_name_of_pid(pid, nm, 64);
            if (!nm[0] || app_known(nm)) continue;
            int dup = 0;
            for (int k = 0; k < n; k++) if (!_wcsicmp(out[k], nm)) dup = 1;
            if (!dup) lstrcpynW(out[n++], nm, 64);
        }
    }
    if (se) IAudioSessionEnumerator_Release(se);
    if (mgr) IAudioSessionManager2_Release(mgr);
    IMMDevice_Release(dev);
    return n;
}

/* ---------------- settings storage ---------------- */
static void reg_rd(HKEY k, const WCHAR *name, int *v, int mn, int mx)
{
    DWORD t, d, sz = sizeof d;
    if (RegQueryValueExW(k, name, NULL, &t, (BYTE *)&d, &sz) == ERROR_SUCCESS && t == REG_DWORD)
        *v = (int)d < mn ? mn : ((int)d > mx ? mx : (int)d);
}
static void reg_wr(HKEY k, const WCHAR *name, int v)
{
    DWORD d = (DWORD)v;
    RegSetValueExW(k, name, 0, REG_DWORD, (BYTE *)&d, sizeof d);
}

static void settings_load(void)
{
    HKEY k;
    settings_defaults(&S);
    nApps = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, APPKEY, 0, KEY_READ, &k)) return;
    reg_rd(k, L"bars", &S.bars, 8, MAXBARS);       reg_rd(k, L"height", &S.heightPct, 5, 100);
    reg_rd(k, L"width", &S.widthPct, 10, 100);     reg_rd(k, L"offset", &S.offset, 0, 400);
    reg_rd(k, L"fps", &S.fps, 10, 60);             reg_rd(k, L"dir", &S.dir, 0, 1);
    reg_rd(k, L"mirror", &S.mirror, 0, 1);         reg_rd(k, L"round", &S.round, 0, 1);
    reg_rd(k, L"glow", &S.glow, 0, 1);             reg_rd(k, L"peaks", &S.peaks, 0, 1);
    reg_rd(k, L"order", &S.order, 0, 2);           reg_rd(k, L"focus", &S.focusPause, 0, 1);
    reg_rd(k, L"wall", &S.useWall, 0, 1);          reg_rd(k, L"apponly", &S.appOnly, 0, 1);
    reg_rd(k, L"attach", &S.attach, 0, 5);
    reg_rd(k, L"preset", &S.preset, 0, NPRESETS - 1);

    int ns = 0;
    DWORD t, d, sz = sizeof d;
    if (RegQueryValueExW(k, L"nstops", NULL, &t, (BYTE *)&d, &sz) == ERROR_SUCCESS && t == REG_DWORD) {
        ns = (int)d;
        if (ns >= 2 && ns <= MAXSTOPS) {
            S.nstops = ns;
            for (int i = 0; i < ns; i++) {
                WCHAR nm[16]; int p = S.spos[i], c = (int)S.scol[i];
                wsprintfW(nm, L"spos%d", i); reg_rd(k, nm, &p, 0, 1000);
                wsprintfW(nm, L"scol%d", i); reg_rd(k, nm, &c, 0, 0xFFFFFF);
                S.spos[i] = p; S.scol[i] = (uint32_t)c;
            }
        }
    } else if (S.preset > 0) {
        apply_preset(&S, S.preset);
    } else {                                     /* settings from an older version */
        int c1 = 0x00E5FF, c2 = 0xFF2BD6;
        reg_rd(k, L"c1", &c1, 0, 0xFFFFFF); reg_rd(k, L"c2", &c2, 0, 0xFFFFFF);
        S.nstops = 2; S.spos[0] = 0; S.spos[1] = 1000; S.scol[0] = (uint32_t)c1; S.scol[1] = (uint32_t)c2;
    }
    fix_stops(&S);

    WCHAR buf[MAXAPPS * 64 + 2] = {0};
    sz = sizeof buf - 4;
    if (RegQueryValueExW(k, L"apps", NULL, &t, (BYTE *)buf, &sz) == ERROR_SUCCESS && t == REG_MULTI_SZ) {
        for (WCHAR *p = buf; *p && nApps < MAXAPPS; p += lstrlenW(p) + 1)
            lstrcpynW(appNames[nApps++], p, 64);
    }
    RegCloseKey(k);
}

static void settings_save(void)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, APPKEY, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL)) return;
    reg_wr(k, L"bars", S.bars);     reg_wr(k, L"height", S.heightPct); reg_wr(k, L"width", S.widthPct);
    reg_wr(k, L"offset", S.offset); reg_wr(k, L"fps", S.fps);          reg_wr(k, L"dir", S.dir);
    reg_wr(k, L"mirror", S.mirror); reg_wr(k, L"round", S.round);      reg_wr(k, L"glow", S.glow);
    reg_wr(k, L"peaks", S.peaks);   reg_wr(k, L"order", S.order);      reg_wr(k, L"focus", S.focusPause);
    reg_wr(k, L"wall", S.useWall);  reg_wr(k, L"apponly", S.appOnly);  reg_wr(k, L"preset", S.preset);
    reg_wr(k, L"attach", S.attach);
    reg_wr(k, L"nstops", S.nstops);
    for (int i = 0; i < S.nstops; i++) {
        WCHAR nm[16];
        wsprintfW(nm, L"spos%d", i); reg_wr(k, nm, S.spos[i]);
        wsprintfW(nm, L"scol%d", i); reg_wr(k, nm, (int)S.scol[i]);
    }
    WCHAR buf[MAXAPPS * 64 + 2] = {0};
    int len = 0;
    for (int i = 0; i < nApps; i++) { lstrcpyW(buf + len, appNames[i]); len += lstrlenW(appNames[i]) + 1; }
    buf[len++] = 0;
    RegSetValueExW(k, L"apps", 0, REG_MULTI_SZ, (BYTE *)buf, len * 2);
    RegCloseKey(k);
}

static int startup_get(void)
{
    HKEY k; int r;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUNKEY, 0, KEY_READ, &k)) return 0;
    r = RegQueryValueExW(k, L"LiteVis", NULL, NULL, NULL, NULL) == ERROR_SUCCESS;
    RegCloseKey(k);
    return r;
}

static void startup_set(int on)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, RUNKEY, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL)) return;
    if (on) {
        WCHAR path[MAX_PATH], cmd[MAX_PATH + 32];
        GetModuleFileNameW(NULL, path, MAX_PATH);
        wsprintfW(cmd, L"\"%s\" --startup", path);
        RegSetValueExW(k, L"LiteVis", 0, REG_SZ, (BYTE *)cmd, (lstrlenW(cmd) + 1) * 2);
    } else RegDeleteValueW(k, L"LiteVis");
    RegCloseKey(k);
}

/* ---------------- background (wallpaper) ---------------- */
static uint32_t reg_solid_color(void)
{
    HKEY k; WCHAR buf[64] = {0}; DWORD sz = sizeof buf, t; int r = 0, g = 0, b = 0;
    if (!RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\Colors", 0, KEY_READ, &k)) {
        if (RegQueryValueExW(k, L"Background", NULL, &t, (BYTE *)buf, &sz) == ERROR_SUCCESS)
            swscanf(buf, L"%d %d %d", &r, &g, &b);
        RegCloseKey(k);
    }
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static int reg_desktop_int(const WCHAR *name, int def)
{
    HKEY k; WCHAR buf[16] = {0}; DWORD sz = sizeof buf, t; int v = def;
    if (!RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", 0, KEY_READ, &k)) {
        if (RegQueryValueExW(k, name, NULL, &t, (BYTE *)buf, &sz) == ERROR_SUCCESS) v = _wtoi(buf);
        RegCloseKey(k);
    }
    return v;
}

static void load_background(void)
{
    uint32_t solid = S.useWall ? reg_solid_color() : 0;
    for (size_t i = 0; i < (size_t)sw * sh; i++) bgBits[i] = solid;
    if (!S.useWall) return;

    WCHAR path[MAX_PATH] = {0};
    SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, path, 0);
    if (!path[0]) return;

    ULONG_PTR tok; GpInput in = {1, NULL, FALSE, FALSE};
    if (GdiplusStartup(&tok, &in, NULL)) return;
    void *img = NULL, *gfx = NULL; UINT iw = 0, ih = 0;
    if (!GdipCreateBitmapFromFile(path, &img) && img) {
        GdipGetImageWidth(img, &iw); GdipGetImageHeight(img, &ih);
        if (iw && ih && !GdipCreateFromHDC(bgDC, &gfx)) {
            GdipSetInterpolationMode(gfx, 7);               /* high quality bicubic */
            int style = reg_desktop_int(L"WallpaperStyle", 10);
            int dx = 0, dy = 0, dw = sw, dh = sh, sx = 0, sy = 0, sW = (int)iw, sH = (int)ih;
            if (style == 2) {                                 /* stretch */
            } else if (style == 6) {                          /* fit */
                float sc = fminf((float)sw / iw, (float)sh / ih);
                dw = (int)(iw * sc); dh = (int)(ih * sc); dx = (sw - dw) / 2; dy = (sh - dh) / 2;
            } else if (style == 0) {                          /* center / tile */
                dw = (int)iw; dh = (int)ih; dx = (sw - dw) / 2; dy = (sh - dh) / 2;
            } else {                                          /* fill / span */
                float sc = fmaxf((float)sw / iw, (float)sh / ih);
                sW = (int)(sw / sc); sH = (int)(sh / sc);
                sx = ((int)iw - sW) / 2; sy = ((int)ih - sH) / 2;
            }
            GdipDrawImageRectRectI(gfx, img, dx, dy, dw, dh, sx, sy, sW, sH, 2, NULL, NULL, NULL);
        }
    }
    if (gfx) GdipDeleteGraphics(gfx);
    if (img) GdipDisposeImage(img);
    GdiplusShutdown(tok);                                     /* frees GDI+ memory again */
}

/* ---------------- surfaces / layout ---------------- */
static void ensure_full(void)
{
    BITMAPINFO bi = {0};
    if (fullBmp) return;
    if (!fullDC) fullDC = CreateCompatibleDC(NULL);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = sw; bi.bmiHeader.biHeight = -sh;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    fullBmp = CreateDIBSection(fullDC, &bi, DIB_RGB_COLORS, (void **)&fullBits, NULL, 0);
    SelectObject(fullDC, fullBmp);
}

static void free_full(void)
{
    if (!fullBmp) return;
    SelectObject(fullDC, GetStockObject(NULL_BRUSH));
    DeleteObject(fullBmp);
    fullBmp = NULL; fullBits = NULL;
}

/* UpdateLayeredWindow path: push rows y0..y1 of the full-screen composite to the layered window */
static void ulw_push(int y0, int y1)
{
    SIZE sz = {sw, sh};
    POINT ps = {0, 0};
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, 0};
    RECT dirty = {0, y0, sw, y1};
    UPDATELAYEREDWINDOWINFO u;
    memset(&u, 0, sizeof u);
    u.cbSize = sizeof u; u.psize = &sz; u.hdcSrc = fullDC; u.pptSrc = &ps;
    u.pblend = &bf; u.dwFlags = ULW_ALPHA; u.prcDirty = &dirty;
    UpdateLayeredWindowIndirect(hWall, &u);
}

static void draw_full(void)                    /* whole screen: wallpaper copy */
{
    if (!hWall || !IsWindow(hWall)) return;
    if (g_ulw) {
        ensure_full();
        memcpy(fullBits, bgBits, (size_t)sw * sh * 4);
        ulw_push(0, sh);
    } else {
        HDC dc = GetDC(hWall);
        BitBlt(dc, 0, 0, sw, sh, bgDC, 0, 0, SRCCOPY);
        ReleaseDC(hWall, dc);
    }
}

static void present_strip(int y0, int y1)      /* rows y0..y1 of the bar strip */
{
    if (!hWall) return;
    if (g_ulw) {
        ensure_full();
        for (int y = y0; y < y1; y++)
            memcpy(fullBits + (size_t)(stripTop + y) * sw, frBits + (size_t)y * sw, (size_t)sw * 4);
        ulw_push(stripTop + y0, stripTop + y1);
    } else {
        HDC dc = GetDC(hWall);
        BitBlt(dc, 0, stripTop + y0, sw, y1 - y0, frDC, 0, y0, SRCCOPY);
        ReleaseDC(hWall, dc);
    }
}

static void make_bg(void)
{
    BITMAPINFO bi = {0};
    if (bgBmp) { SelectObject(bgDC, GetStockObject(NULL_BRUSH)); DeleteObject(bgBmp); }
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = sw; bi.bmiHeader.biHeight = -sh;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    bgBmp = CreateDIBSection(bgDC, &bi, DIB_RGB_COLORS, (void **)&bgBits, NULL, 0);
    SelectObject(bgDC, bgBmp);
}

static void apply_layout(void)
{
    int nh = sh * S.heightPct / 100;
    if (nh < 16) nh = 16;
    int top = sh - S.offset - nh;
    if (top < 0) { top = 0; nh = sh - S.offset; if (nh < 16) nh = 16; }
    int moved = (top != stripTop || nh != stripH);
    if (nh != stripH || !frBmp) {
        BITMAPINFO bi = {0};
        stripH = nh;
        if (frBmp) { SelectObject(frDC, GetStockObject(NULL_BRUSH)); DeleteObject(frBmp); }
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = sw; bi.bmiHeader.biHeight = -stripH;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
        frBmp = CreateDIBSection(frDC, &bi, DIB_RGB_COLORS, (void **)&frBits, NULL, 0);
        SelectObject(frDC, frBmp);
        free(cv.rowRGB);
        cv.rowRGB = (uint32_t *)malloc((size_t)stripH * 4);
    }
    stripTop = top;
    cv.W = sw; cv.H = stripH; cv.frame = frBits;
    cv.bg = bgBits + (size_t)stripTop * sw;
    canvas_colors(&S, &cv);
    if (moved) draw_full();                                   /* erase bars left at the old position */
    static int lastFps;
    if (hCtl && S.fps != lastFps) { SetTimer(hCtl, 1, 1000 / S.fps, NULL); lastFps = S.fps; }
}

/* ---------------- wallpaper-layer window ---------------- */
static BOOL CALLBACK enum_proc(HWND h, LPARAM lp)
{
    if (FindWindowExW(h, NULL, L"SHELLDLL_DefView", NULL))
        *(HWND *)lp = FindWindowExW(NULL, h, L"WorkerW", NULL);
    return TRUE;
}

static LRESULT CALLBACK wall_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        if (!g_ulw) BitBlt(dc, 0, 0, sw, sh, bgDC, 0, 0, SRCCOPY);
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_ERASEBKGND) return 1;
    return DefWindowProcW(h, m, w, l);
}

static void create_wall(void);
static int g_mode;                       /* 1/3/4 = Win10-style layout, 2 = Windows 11 24H2 layout */

/* Windows 11 24H2: the window must sit  DefView (icons) > ours > WorkerW (wallpaper)  inside Progman */
static void fix_zorder(void)
{
    HWND progman, defview, wchild;
    if (g_mode != 2 || !hWall || !IsWindow(hWall)) return;
    progman = FindWindowW(L"Progman", NULL);
    if (GetParent(hWall) != progman) { create_wall(); return; }          /* shell replaced its windows */
    defview = FindWindowExW(progman, NULL, L"SHELLDLL_DefView", NULL);
    wchild = FindWindowExW(progman, NULL, L"WorkerW", NULL);
    int ok = 1, sawDef = (defview == NULL);
    for (HWND w = GetWindow(hWall, GW_HWNDPREV); w; w = GetWindow(w, GW_HWNDPREV)) {
        if (w == wchild) ok = 0;                                         /* wallpaper layer is covering us */
        if (w == defview) sawDef = 1;
    }
    if (!sawDef) ok = 0;
    if (ok) return;
    lv_log("fix_zorder: re-stacking");
    if (defview) SetWindowPos(hWall, defview, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE);
    if (wchild)  SetWindowPos(wchild, hWall, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE);
    draw_full();
    cv.py0 = 0; cv.py1 = cv.H;
}

typedef HANDLE DPICTX;
static DPICTX (WINAPI *pSetThreadDpiCtx)(DPICTX);

static void log_children(HWND p)
{
    int i = 0;
    if (!g_log) return;
    for (HWND h = GetWindow(p, GW_CHILD); h && i < 12; h = GetWindow(h, GW_HWNDNEXT), i++) {
        WCHAR cls[64] = {0}; RECT r = {0};
        GetClassNameW(h, cls, 64); GetWindowRect(h, &r);
        lv_log("  child[%d] (top to bottom) %p class=%ls vis=%d ex=%08lx rect=%ld,%ld,%ld,%ld", i, (void *)h, cls,
               IsWindowVisible(h), (unsigned long)GetWindowLongW(h, GWL_EXSTYLE), r.left, r.top, r.right, r.bottom);
    }
}

typedef struct { int parent; int layered; int ulw; } Method;   /* parent: 0 = Progman, 1 = Progman's WorkerW */
static const Method METHODS[4] = {{0, 1, 0}, {0, 1, 1}, {1, 0, 0}, {1, 1, 1}};
static int g_autoMethod = -1;

static void build_wall(int layout, int mi)
{
    if (hWall && IsWindow(hWall)) DestroyWindow(hWall);
    HWND progman = FindWindowW(L"Progman", NULL), parent = NULL, worker = NULL;
    HWND wchild = FindWindowExW(progman, NULL, L"WorkerW", NULL);        /* child of Progman => 24H2+ layout */
    HWND defview = FindWindowExW(progman, NULL, L"SHELLDLL_DefView", NULL);
    Method m = {0, 0, 0};
    int mode;
    if (layout == 1) {                                                   /* Windows 11 methods */
        m = METHODS[mi];
        parent = (m.parent == 1 && wchild) ? wchild : progman;
        mode = parent == progman ? 2 : 5;
    } else {                                                             /* Windows 10 style */
        EnumWindows(enum_proc, (LPARAM)&worker);
        if (worker) { parent = worker; mode = 1; }
        else { parent = wchild ? wchild : progman; mode = wchild ? 3 : 4; }
    }
    g_mode = mode; g_ulw = m.ulw;
    if (!g_ulw) free_full();
    int wx = -GetSystemMetrics(SM_XVIRTUALSCREEN), wy = -GetSystemMetrics(SM_YVIRTUALSCREEN);

    /* explorer is per-monitor DPI aware on Windows 11: host our window with the same awareness */
    DPICTX oldCtx = NULL;
    if (mode != 1 && pSetThreadDpiCtx) oldCtx = pSetThreadDpiCtx((DPICTX)(INT_PTR)-4);

    DWORD exCreated, exAfter, le1 = 0, le2 = 0, le3 = 0;
    int layeredOK = 1;
    hWall = CreateWindowExW(m.layered ? WS_EX_LAYERED : 0, L"LiteVisWall", L"LatteVisualizer", WS_POPUP,
                            0, 0, sw, sh, NULL, NULL, hInst, NULL);
    exCreated = (DWORD)GetWindowLongW(hWall, GWL_EXSTYLE);
    if (m.layered && !m.ulw) SetLayeredWindowAttributes(hWall, 0, 255, LWA_ALPHA);
    if (layout == 1) {                   /* reparent first, then turn into a child */
        SetLastError(0); SetParent(hWall, parent); le1 = GetLastError();
        SetLastError(0); SetWindowLongW(hWall, GWL_STYLE, WS_CHILD | WS_VISIBLE); le2 = GetLastError();
    } else {
        SetWindowLongW(hWall, GWL_STYLE, WS_CHILD | WS_VISIBLE);
        SetLastError(0); SetParent(hWall, parent); le1 = GetLastError();
    }
    if (m.layered) {                     /* the layered flag has to survive the reparenting */
        if (!(GetWindowLongW(hWall, GWL_EXSTYLE) & WS_EX_LAYERED))
            SetWindowLongW(hWall, GWL_EXSTYLE, GetWindowLongW(hWall, GWL_EXSTYLE) | WS_EX_LAYERED);
        if (!m.ulw) {
            SetLastError(0);
            BOOL la = SetLayeredWindowAttributes(hWall, 0, 255, LWA_ALPHA);
            le3 = GetLastError();
            layeredOK = la != 0;
        }
        layeredOK = layeredOK && (GetWindowLongW(hWall, GWL_EXSTYLE) & WS_EX_LAYERED) != 0;
    }
    if (mode == 2) {
        SetWindowPos(hWall, defview ? defview : HWND_TOP, wx, wy, sw, sh,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_FRAMECHANGED);
        if (wchild) SetWindowPos(wchild, hWall, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE);
    } else {
        SetWindowPos(hWall, NULL, wx, wy, sw, sh, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_FRAMECHANGED);
    }
    exAfter = (DWORD)GetWindowLongW(hWall, GWL_EXSTYLE);
    if (oldCtx) pSetThreadDpiCtx(oldCtx);

    RECT wr = {0}; GetWindowRect(hWall, &wr);
    lv_log("create_wall: attach=%d layout=%d method=%d(layered=%d ulw=%d) mode=%d progman=%p parent=%p defview=%p workerwChild=%p wall=%p realParent=%p visible=%d rect=%ld,%ld,%ld,%ld size=%dx%d",
           S.attach, layout, mi + 1, m.layered, m.ulw, mode, (void *)progman, (void *)parent, (void *)defview,
           (void *)wchild, (void *)hWall, (void *)GetParent(hWall), IsWindowVisible(hWall),
           wr.left, wr.top, wr.right, wr.bottom, sw, sh);
    lv_log("  styles: exCreated=%08lx exAfter=%08lx layeredOK=%d errors: setParent=%lu setStyle=%lu setLayered=%lu",
           (unsigned long)exCreated, (unsigned long)exAfter, layeredOK, (unsigned long)le1, (unsigned long)le2, (unsigned long)le3);
    log_children(progman);
    draw_full();
    cv.py0 = 0; cv.py1 = cv.H;
}

/* Paints magenta squares and reads the real screen back: is our window actually on screen?
   returns 1 = visible, 0 = not visible, -1 = desktop not exposed (other windows in the way) */
static int probe_visible(void)
{
    enum { N = 5, B = 24 };
    POINT p[N];
    static uint32_t saved[N][B * B];
    int exposed = 0, ok = 0;
    for (int i = 0; i < N; i++) {
        p[i].x = sw * (i + 1) / (N + 1); p[i].y = sh / 3 + (i % 2) * 40;
        for (int y = 0; y < B; y++)
            for (int x = 0; x < B; x++) {
                uint32_t *px = bgBits + (size_t)(p[i].y + y) * sw + p[i].x + x;
                saved[i][y * B + x] = *px; *px = 0xFF00FF;
            }
    }
    draw_full();
    Sleep(350);                                                   /* let the compositor catch up */
    HDC scr = GetDC(NULL);
    for (int i = 0; i < N; i++) {
        POINT pt = {p[i].x + B / 2, p[i].y + B / 2};
        HWND w = WindowFromPoint(pt), root = w ? GetAncestor(w, GA_ROOT) : NULL;
        WCHAR cls[64] = L"";
        if (root) GetClassNameW(root, cls, 64);
        if (lstrcmpW(cls, L"Progman") && lstrcmpW(cls, L"WorkerW")) continue;   /* a normal window covers it */
        exposed++;
        COLORREF col = GetPixel(scr, pt.x, pt.y);
        if (col != CLR_INVALID && GetRValue(col) > 220 && GetGValue(col) < 40 && GetBValue(col) > 220) ok++;
    }
    ReleaseDC(NULL, scr);
    for (int i = 0; i < N; i++)
        for (int y = 0; y < B; y++)
            for (int x = 0; x < B; x++)
                bgBits[(size_t)(p[i].y + y) * sw + p[i].x + x] = saved[i][y * B + x];
    draw_full();
    lv_log("probe: exposed=%d matches=%d", exposed, ok);
    return exposed == 0 ? -1 : (ok > 0);
}

static void create_wall(void)
{
	HWND progman = FindWindowW(L"Progman", NULL);
    SendMessageTimeoutW(progman, 0x052C, 0, 0, SMTO_NORMAL, 1000, NULL);
    int win11 = FindWindowExW(progman, NULL, L"WorkerW", NULL) || FindWindowExW(progman, NULL, L"SHELLDLL_DefView", NULL);

    if (S.attach == 5 || (S.attach == 0 && !win11)) { build_wall(0, 0); return; }
    if (S.attach >= 1 && S.attach <= 4) { build_wall(1, S.attach - 1); return; }
    if (g_autoMethod >= 0) { build_wall(1, g_autoMethod); return; }

    /* Auto on Windows 11: try each method and keep the first one that really shows up on screen */
    for (int mi = 0; mi < 4; mi++) {
        build_wall(1, mi);
        int r = probe_visible();
        lv_log("auto: method %d -> %s", mi + 1, r > 0 ? "VISIBLE" : (r == 0 ? "not visible" : "inconclusive"));
        if (r > 0) { g_autoMethod = mi; return; }
        if (r < 0) { build_wall(1, 0); return; }               /* can't tell: use the default, retry later */
    }
    g_autoMethod = 0;                                           /* none confirmed: default */
    build_wall(1, 0);
}

/* ---------------- icon / tray ---------------- */
static HICON make_icon(int size)
{
    HICON res = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(2), IMAGE_ICON, size, size, 0);   /* embedded icon */
    if (res) return res;
    BITMAPINFO bi = {0};
    uint32_t *px;
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = size; bi.bmiHeader.biHeight = -size;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    HDC dc = GetDC(NULL);
    HBITMAP color = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0);
    ReleaseDC(NULL, dc);
    memset(px, 0, (size_t)size * size * 4);
    static const float hts[5] = {0.45f, 0.80f, 0.60f, 0.95f, 0.55f};
    int bw = size / 7 > 1 ? size / 7 : 1, gap = bw / 2 > 1 ? bw / 2 : 1;
    int total = 5 * bw + 4 * gap, x0 = (size - total) / 2;
    for (int i = 0; i < 5; i++) {
        int h = (int)(hts[i] * (size - 2)), x = x0 + i * (bw + gap);
        for (int y = size - 1 - h; y < size - 1 && y >= 0; y++) {
            float t = (float)(size - 1 - y) / (size - 1);
            uint32_t c = lerp_rgb(0x00E5FF, 0xFF2BD6, t);
            for (int xx = x; xx < x + bw && xx < size; xx++) px[y * size + xx] = 0xFF000000u | c;
        }
    }
    HBITMAP mask = CreateBitmap(size, size, 1, 1, NULL);
    ICONINFO ii = {TRUE, 0, 0, mask, color};
    HICON ic = CreateIconIndirect(&ii);
    DeleteObject(mask); DeleteObject(color);
    return ic;
}

static void tray_add(void)
{
    memset(&nid, 0, sizeof nid);
    nid.cbSize = sizeof nid; nid.hWnd = hCtl; nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = make_icon(GetSystemMetrics(SM_CXSMICON));
    lstrcpyW(nid.szTip, L"Latte Visualizer");
    Shell_NotifyIconW(NIM_ADD, &nid);
}

/* ---------------- settings window ---------------- */
enum {
    ID_STARTUP = 100,
    ID_BARS = 110, ID_HEIGHT = 120, ID_WIDTH = 130, ID_OFFSET = 140, ID_FPS = 150,
    ID_PRESET = 160, ID_DIR = 163, ID_ORDER = 165, ID_GRAD = 166, ID_STOPCOL = 167,
    ID_STOPPOS = 168, ID_ADDSTOP = 169, ID_DELSTOP = 190,
    ID_MIRROR = 170, ID_ROUND = 171, ID_GLOW = 172, ID_PEAKS = 173, ID_WALL = 174, ID_FOCUS = 175,
    ID_APPONLY = 176, ID_APPLIST = 177, ID_APPADD = 178, ID_APPDEL = 179,
    ID_RESET = 180, ID_CLOSE = 181, ID_TEST = 182, ID_ATTACH = 183
};

static int selStop;

/* Test bars: in Auto mode re-run the on-screen detection first, then play the demo */
static void start_test(void)
{
    paused = 0;
    if (S.attach == 0) { g_autoMethod = -1; create_wall(); }
    demoLeft = 5.f; demoT = 0.f;
}
static void update_controls(void);
static void on_change(int bg);

static HWND mk(const WCHAR *cls, const WCHAR *txt, DWORD style, int x, int y, int w, int h, int id)
{
    return CreateWindowExW(0, cls, txt, WS_CHILD | WS_VISIBLE | style, SC(x), SC(y), SC(w), SC(h),
                           hSet, (HMENU)(INT_PTR)id, hInst, NULL);
}

static void mk_slider(const WCHAR *label, int y, int id, int mn, int mx)
{
    mk(L"STATIC", label, SS_LEFT, 16, y + 4, 110, 20, 0);
    HWND t = mk(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, 124, y, 196, 28, id);
    SendMessageW(t, TBM_SETRANGE, TRUE, MAKELONG(mn, mx));
    mk(L"STATIC", L"", SS_RIGHT, 322, y + 4, 62, 20, id + 1);
}

static void set_slider(int id, int v, const WCHAR *fmt)
{
    WCHAR b[32];
    SendDlgItemMessageW(hSet, id, TBM_SETPOS, TRUE, v);
    wsprintfW(b, fmt, v);
    SetDlgItemTextW(hSet, id + 1, b);
}

static COLORREF u2c(uint32_t u) { return RGB((u >> 16) & 255, (u >> 8) & 255, u & 255); }
static uint32_t c2u(COLORREF c) { return ((uint32_t)GetRValue(c) << 16) | ((uint32_t)GetGValue(c) << 8) | GetBValue(c); }

static void stop_ui(void)
{
    WCHAR b[48];
    if (selStop >= S.nstops) selStop = S.nstops - 1;
    if (selStop < 0) selStop = 0;
    wsprintfW(b, L"Position: %d %%", S.spos[selStop] / 10);
    SetDlgItemTextW(hSet, ID_STOPPOS, b);
    EnableWindow(GetDlgItem(hSet, ID_ADDSTOP), S.nstops < MAXSTOPS);
    EnableWindow(GetDlgItem(hSet, ID_DELSTOP), S.nstops > 2);
    InvalidateRect(GetDlgItem(hSet, ID_STOPCOL), NULL, TRUE);
    InvalidateRect(GetDlgItem(hSet, ID_GRAD), NULL, TRUE);
}

static void apps_refresh_list(void)
{
    SendDlgItemMessageW(hSet, ID_APPLIST, LB_RESETCONTENT, 0, 0);
    for (int i = 0; i < nApps; i++) SendDlgItemMessageW(hSet, ID_APPLIST, LB_ADDSTRING, 0, (LPARAM)appNames[i]);
}

static void update_controls(void)
{
    set_slider(ID_BARS, S.bars, L"%d");
    set_slider(ID_HEIGHT, S.heightPct, L"%d %%");
    set_slider(ID_WIDTH, S.widthPct, L"%d %%");
    set_slider(ID_OFFSET, S.offset, L"%d px");
    set_slider(ID_FPS, S.fps, L"%d");
    CheckDlgButton(hSet, ID_STARTUP, startup_get() ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hSet, ID_MIRROR, S.mirror ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hSet, ID_ROUND, S.round ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hSet, ID_GLOW, S.glow ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hSet, ID_PEAKS, S.peaks ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hSet, ID_WALL, S.useWall ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hSet, ID_FOCUS, S.focusPause ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hSet, ID_APPONLY, S.appOnly ? BST_CHECKED : BST_UNCHECKED);
    SendDlgItemMessageW(hSet, ID_PRESET, CB_SETCURSEL, S.preset, 0);
    SendDlgItemMessageW(hSet, ID_DIR, CB_SETCURSEL, S.dir, 0);
    SendDlgItemMessageW(hSet, ID_ORDER, CB_SETCURSEL, S.order, 0);
    SendDlgItemMessageW(hSet, ID_ATTACH, CB_SETCURSEL, S.attach, 0);
    apps_refresh_list();
    stop_ui();
}

static void on_change(int bg)
{
    if (bg) { load_background(); draw_full(); }
    apply_layout();
    if (hCtl) SetTimer(hCtl, 2, 800, NULL);                   /* debounced save */
}

static void pick_stop_color(int i)
{
    static COLORREF custom[16];
    CHOOSECOLORW c = {0};
    c.lStructSize = sizeof c; c.hwndOwner = hSet; c.rgbResult = u2c(S.scol[i]);
    c.lpCustColors = custom; c.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (ChooseColorW(&c)) {
        S.scol[i] = c2u(c.rgbResult);
        S.preset = 0;
        update_controls();
        on_change(0);
    }
}

/* ---- gradient editor: strip + draggable color markers ---- */
#define GE_PAD 12
static int ge_x(const RECT *rc, int pos)
{
    int w = rc->right - rc->left - 2 * SC(GE_PAD);
    return rc->left + SC(GE_PAD) + (int)(w * (pos / 1000.0));
}
static int ge_hit(HWND h, int x)
{
    RECT rc; GetClientRect(h, &rc);
    int best = -1, bd = SC(11) + 1;
    for (int i = 0; i < S.nstops; i++) {
        int d = abs(ge_x(&rc, S.spos[i]) - x);
        if (d <= bd) { bd = d; best = i; }                     /* ties -> later marker */
    }
    return best;
}

static LRESULT CALLBACK ge_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    static int drag = -1;
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        HDC md = CreateCompatibleDC(dc);
        HBITMAP bm = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ old = SelectObject(md, bm);
        FillRect(md, &rc, (HBRUSH)(COLOR_BTNFACE + 1));
        int sy0 = SC(4), sy1 = rc.bottom - SC(26);
        int x0 = ge_x(&rc, 0), x1 = ge_x(&rc, 1000);
        SelectObject(md, GetStockObject(DC_BRUSH));
        for (int x = x0; x <= x1; x++) {                       /* same colors the bars use */
            float t = x1 > x0 ? (float)(x - x0) / (x1 - x0) : 0.f;
            SetDCBrushColor(md, u2c(g_lut[(int)(t * 255)]));
            PatBlt(md, x, sy0, 1, sy1 - sy0, PATCOPY);
        }
        RECT sr = {x0 - 1, sy0 - 1, x1 + 1, sy1 + 1};
        FrameRect(md, &sr, (HBRUSH)GetStockObject(GRAY_BRUSH));
        for (int i = 0; i < S.nstops; i++) {
            int x = ge_x(&rc, S.spos[i]);
            HPEN pen = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
            HGDIOBJ op = SelectObject(md, pen);
            MoveToEx(md, x, sy0, NULL); LineTo(md, x, sy1 + SC(5));
            SelectObject(md, op); DeleteObject(pen);
            RECT mr = {x - SC(9), sy1 + SC(5), x + SC(9), rc.bottom - SC(2)};
            HBRUSH fb = CreateSolidBrush(u2c(S.scol[i]));
            HBRUSH bb = CreateSolidBrush(i == selStop ? RGB(0, 0, 0) : RGB(120, 120, 120));
            FillRect(md, &mr, fb);
            FrameRect(md, &mr, bb);
            if (i == selStop) { InflateRect(&mr, -1, -1); FrameRect(md, &mr, bb); }
            DeleteObject(fb); DeleteObject(bb);
        }
        BitBlt(dc, 0, 0, rc.right, rc.bottom, md, 0, 0, SRCCOPY);
        SelectObject(md, old); DeleteObject(bm); DeleteDC(md);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_LBUTTONDOWN: {
        int i = ge_hit(h, (short)LOWORD(l));
        if (i >= 0) { selStop = i; drag = i; SetCapture(h); stop_ui(); }
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        int i = ge_hit(h, (short)LOWORD(l));
        if (i >= 0) { selStop = i; stop_ui(); pick_stop_color(i); }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (drag >= 0) {
            RECT rc; GetClientRect(h, &rc);
            int x0 = ge_x(&rc, 0), x1 = ge_x(&rc, 1000);
            int pos = (int)(((short)LOWORD(l) - x0) * 1000.0 / (x1 - x0) + 0.5);
            int lo = drag > 0 ? S.spos[drag - 1] + 10 : 0;
            int hi = drag < S.nstops - 1 ? S.spos[drag + 1] - 10 : 1000;
            if (pos < lo) pos = lo;
            if (pos > hi) pos = hi;
            if (pos != S.spos[drag]) {
                S.spos[drag] = pos; S.preset = 0;
                SendDlgItemMessageW(hSet, ID_PRESET, CB_SETCURSEL, 0, 0);
                on_change(0);
                stop_ui();
            }
        }
        return 0;
    case WM_LBUTTONUP:
        if (drag >= 0) { drag = -1; ReleaseCapture(); }
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void add_stop(void)
{
    if (S.nstops >= MAXSTOPS) return;
    int bi = 0, bg = 0;
    for (int i = 0; i < S.nstops - 1; i++)                    /* largest gap */
        if (S.spos[i + 1] - S.spos[i] > bg) { bg = S.spos[i + 1] - S.spos[i]; bi = i; }
    if (bg < 20) return;
    int pos = S.spos[bi] + bg / 2;
    uint32_t col = grad_at(&S, pos / 1000.f);
    for (int i = S.nstops; i > bi + 1; i--) { S.spos[i] = S.spos[i - 1]; S.scol[i] = S.scol[i - 1]; }
    S.spos[bi + 1] = pos; S.scol[bi + 1] = col;
    S.nstops++;
    selStop = bi + 1;
    S.preset = 0;
    update_controls(); on_change(0);
}

static void del_stop(void)
{
    if (S.nstops <= 2) return;
    for (int i = selStop; i < S.nstops - 1; i++) { S.spos[i] = S.spos[i + 1]; S.scol[i] = S.scol[i + 1]; }
    S.nstops--;
    if (selStop >= S.nstops) selStop = S.nstops - 1;
    S.preset = 0;
    update_controls(); on_change(0);
}

/* ---- per-app list ---- */
static void app_add(const WCHAR *name)
{
    if (!name[0] || nApps >= MAXAPPS || app_known(name)) return;
    lstrcpynW(appNames[nApps++], name, 64);
    apps_refresh_list();
    audio_apply_mode();
    on_change(0);
}

static void app_browse(void)
{
    WCHAR file[MAX_PATH] = {0};
    OPENFILENAMEW o = {0};
    o.lStructSize = sizeof o; o.hwndOwner = hSet;
    o.lpstrFilter = L"Programs (*.exe)\0*.exe\0\0";
    o.lpstrFile = file; o.nMaxFile = MAX_PATH;
    o.lpstrTitle = L"Choose the app's .exe";
    o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (GetOpenFileNameW(&o)) {
        WCHAR *b = wcsrchr(file, L'\\');
        app_add(b ? b + 1 : file);
    }
}

static void app_add_menu(void)
{
    static WCHAR names[12][64];
    int n = list_audio_apps(names, 12);
    HMENU m = CreatePopupMenu();
    RECT r; GetWindowRect(GetDlgItem(hSet, ID_APPADD), &r);
    for (int i = 0; i < n; i++) AppendMenuW(m, MF_STRING, 2000 + i, names[i]);
    if (!n) AppendMenuW(m, MF_STRING | MF_GRAYED, 1, L"(no app is playing audio right now)");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, 1999, L"Browse for .exe...");
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, r.left, r.bottom, 0, hSet, NULL);
    DestroyMenu(m);
    if (cmd == 1999) app_browse();
    else if (cmd >= 2000 && cmd < 2000 + n) app_add(names[cmd - 2000]);
}

static void app_remove(void)
{
    int i = (int)SendDlgItemMessageW(hSet, ID_APPLIST, LB_GETCURSEL, 0, 0);
    if (i < 0 || i >= nApps) return;
    for (int k = i; k < nApps - 1; k++) lstrcpyW(appNames[k], appNames[k + 1]);
    nApps--;
    apps_refresh_list();
    audio_apply_mode();
    on_change(0);
}

static void draw_item(DRAWITEMSTRUCT *d)
{
    if (d->CtlID != ID_STOPCOL) return;
    RECT r = d->rcItem;
    HBRUSH b = CreateSolidBrush(u2c(S.scol[selStop]));
    HBRUSH f = CreateSolidBrush(GetSysColor(COLOR_BTNSHADOW));
    FrameRect(d->hDC, &r, f);
    InflateRect(&r, -1, -1);
    FillRect(d->hDC, &r, b);
    if (d->itemState & ODS_FOCUS) { InflateRect(&r, -2, -2); DrawFocusRect(d->hDC, &r); }
    DeleteObject(b); DeleteObject(f);
}

static void tray_menu(void)
{
    POINT p; GetCursorPos(&p);
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, L"Settings...");
    AppendMenuW(m, MF_STRING, 2, paused ? L"Resume" : L"Pause");
    AppendMenuW(m, MF_STRING, 4, L"Test bars (5 s)");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, 3, L"Exit");
    SetForegroundWindow(hCtl);
    int cmd = TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_BOTTOMALIGN, p.x, p.y, 0, hCtl, NULL);
    DestroyMenu(m);
    PostMessageW(hCtl, WM_NULL, 0, 0);
    if (cmd == 1) { update_controls(); ShowWindow(hSet, SW_SHOW); SetForegroundWindow(hSet); }
    else if (cmd == 2) paused = !paused;
    else if (cmd == 4) start_test();
    else if (cmd == 3) PostMessageW(hCtl, WM_CLOSE, 0, 0);
}

static LRESULT CALLBACK set_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_HSCROLL: {
        int id = GetDlgCtrlID((HWND)l), v = (int)SendMessageW((HWND)l, TBM_GETPOS, 0, 0);
        switch (id) {
        case ID_BARS:   S.bars = v;      set_slider(id, v, L"%d");      break;
        case ID_HEIGHT: S.heightPct = v; set_slider(id, v, L"%d %%");   break;
        case ID_WIDTH:  S.widthPct = v;  set_slider(id, v, L"%d %%");   break;
        case ID_OFFSET: S.offset = v;    set_slider(id, v, L"%d px");   break;
        case ID_FPS:    S.fps = v;       set_slider(id, v, L"%d");      break;
        default: return 0;
        }
        on_change(0);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(w), note = HIWORD(w);
        if (id == ID_STARTUP) { startup_set(IsDlgButtonChecked(h, id) == BST_CHECKED); return 0; }
        if (id == ID_MIRROR || id == ID_ROUND || id == ID_GLOW || id == ID_PEAKS || id == ID_WALL ||
            id == ID_FOCUS || id == ID_APPONLY) {
            int on = IsDlgButtonChecked(h, id) == BST_CHECKED;
            if (id == ID_MIRROR) S.mirror = on;
            else if (id == ID_ROUND) S.round = on;
            else if (id == ID_GLOW) S.glow = on;
            else if (id == ID_PEAKS) S.peaks = on;
            else if (id == ID_FOCUS) S.focusPause = on;
            else if (id == ID_APPONLY) { S.appOnly = on; audio_apply_mode(); }
            else { S.useWall = on; on_change(1); return 0; }
            on_change(0);
            return 0;
        }
        if (id == ID_PRESET && note == CBN_SELCHANGE) {
            int i = (int)SendDlgItemMessageW(h, ID_PRESET, CB_GETCURSEL, 0, 0);
            S.preset = i;
            if (i > 0) { apply_preset(&S, i); selStop = 0; }
            update_controls(); on_change(0);
            return 0;
        }
        if (id == ID_DIR && note == CBN_SELCHANGE) {
            S.dir = (int)SendDlgItemMessageW(h, ID_DIR, CB_GETCURSEL, 0, 0);
            on_change(0);
            return 0;
        }
        if (id == ID_ORDER && note == CBN_SELCHANGE) {
            S.order = (int)SendDlgItemMessageW(h, ID_ORDER, CB_GETCURSEL, 0, 0);
            on_change(0);
            return 0;
        }
        if (id == ID_ATTACH && note == CBN_SELCHANGE) {
            S.attach = (int)SendDlgItemMessageW(h, ID_ATTACH, CB_GETCURSEL, 0, 0);
            if (S.attach == 0) g_autoMethod = -1;
            create_wall();
            on_change(0);
            return 0;
        }
        if (id == ID_TEST) { start_test(); return 0; }
        if (id == ID_STOPCOL && note == BN_CLICKED) { pick_stop_color(selStop); return 0; }
        if (id == ID_ADDSTOP) { add_stop(); return 0; }
        if (id == ID_DELSTOP) { del_stop(); return 0; }
        if (id == ID_APPADD) { app_add_menu(); return 0; }
        if (id == ID_APPDEL) { app_remove(); return 0; }
        if (id == ID_RESET) {
            settings_defaults(&S);
            selStop = 0;
            update_controls(); audio_apply_mode(); create_wall(); on_change(1);
            return 0;
        }
        if (id == ID_CLOSE) { ShowWindow(h, SW_HIDE); return 0; }
        return 0;
    }
    case WM_DRAWITEM: draw_item((DRAWITEMSTRUCT *)l); return TRUE;
    case WM_CLOSE: ShowWindow(h, SW_HIDE); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static BOOL CALLBACK font_proc(HWND c, LPARAM f) { SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE); return TRUE; }

static void create_settings(void)
{
    NONCLIENTMETRICSW ncm = {sizeof ncm};
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    uiFont = CreateFontIndirectW(&ncm.lfMessageFont);

    RECT r = {0, 0, SC(800), SC(476)};
    DWORD st = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&r, st, FALSE);
    hSet = CreateWindowExW(0, L"LiteVisSet", L"Latte Visualizer Settings", st,
                           (sw - (r.right - r.left)) / 2, (sh - (r.bottom - r.top)) / 2,
                           r.right - r.left, r.bottom - r.top, NULL, NULL, hInst, NULL);

    /* ---- left column: bars ---- */
    mk(L"BUTTON", L"Start with Windows", BS_AUTOCHECKBOX | WS_TABSTOP, 16, 12, 250, 22, ID_STARTUP);
    mk_slider(L"Bars amount", 44, ID_BARS, 8, MAXBARS);
    mk_slider(L"Bars height", 78, ID_HEIGHT, 5, 100);
    mk_slider(L"Bars width", 112, ID_WIDTH, 10, 100);
    mk_slider(L"Bottom offset", 146, ID_OFFSET, 0, 400);
    mk_slider(L"Frame rate", 180, ID_FPS, 10, 60);

    mk(L"STATIC", L"Bar order", SS_LEFT, 16, 222, 100, 20, 0);
    HWND oc = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 124, 218, 220, 140, ID_ORDER);
    SendMessageW(oc, CB_ADDSTRING, 0, (LPARAM)L"Left to right");
    SendMessageW(oc, CB_ADDSTRING, 0, (LPARAM)L"Right to left");
    SendMessageW(oc, CB_ADDSTRING, 0, (LPARAM)L"Centered (bass in middle)");

    mk(L"BUTTON", L"Mirrored", BS_AUTOCHECKBOX | WS_TABSTOP, 16, 254, 110, 22, ID_MIRROR);
    mk(L"BUTTON", L"Rounded bars", BS_AUTOCHECKBOX | WS_TABSTOP, 136, 254, 120, 22, ID_ROUND);
    mk(L"BUTTON", L"Glow", BS_AUTOCHECKBOX | WS_TABSTOP, 270, 254, 110, 22, ID_GLOW);
    mk(L"BUTTON", L"Peak caps", BS_AUTOCHECKBOX | WS_TABSTOP, 16, 282, 110, 22, ID_PEAKS);
    mk(L"BUTTON", L"Show my wallpaper (off = black)", BS_AUTOCHECKBOX | WS_TABSTOP, 136, 282, 250, 22, ID_WALL);
    mk(L"BUTTON", L"Pause when another window is focused", BS_AUTOCHECKBOX | WS_TABSTOP, 16, 310, 368, 22, ID_FOCUS);

    mk(L"STATIC", L"Desktop attach", SS_LEFT, 16, 346, 100, 20, 0);
    HWND ac2 = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 124, 342, 260, 200, ID_ATTACH);
    SendMessageW(ac2, CB_ADDSTRING, 0, (LPARAM)L"Auto (recommended)");
    SendMessageW(ac2, CB_ADDSTRING, 0, (LPARAM)L"Windows 11 A - under the icons");
    SendMessageW(ac2, CB_ADDSTRING, 0, (LPARAM)L"Windows 11 B - under the icons, other drawing");
    SendMessageW(ac2, CB_ADDSTRING, 0, (LPARAM)L"Windows 11 C - inside the wallpaper layer");
    SendMessageW(ac2, CB_ADDSTRING, 0, (LPARAM)L"Windows 11 D - wallpaper layer, other drawing");
    SendMessageW(ac2, CB_ADDSTRING, 0, (LPARAM)L"Windows 10 style");
    mk(L"STATIC", L"Auto tests each method on your screen and keeps the first that works. Press Test bars to re-run it. Only change this if Auto shows nothing.",
       SS_LEFT, 16, 374, 368, 44, 0);

    /* ---- right column: gradient ---- */
    const int rx = 412;
    mk(L"STATIC", L"Gradient", SS_LEFT, rx, 16, 70, 20, 0);
    HWND pc = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, rx + 72, 12, 140, 240, ID_PRESET);
    for (int i = 0; i < NPRESETS; i++) SendMessageW(pc, CB_ADDSTRING, 0, (LPARAM)PRESETS[i].name);
    HWND dc = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, rx + 222, 12, 150, 120, ID_DIR);
    SendMessageW(dc, CB_ADDSTRING, 0, (LPARAM)L"Colors by height");
    SendMessageW(dc, CB_ADDSTRING, 0, (LPARAM)L"Colors across bars");

    mk(L"GradEditor", L"", WS_TABSTOP, rx, 46, 372, 54, ID_GRAD);
    mk(L"BUTTON", L"", BS_OWNERDRAW | WS_TABSTOP, rx, 108, 60, 26, ID_STOPCOL);
    mk(L"STATIC", L"", SS_LEFT, rx + 68, 112, 104, 20, ID_STOPPOS);
    mk(L"BUTTON", L"Add color", BS_PUSHBUTTON | WS_TABSTOP, rx + 176, 108, 92, 26, ID_ADDSTOP);
    mk(L"BUTTON", L"Remove color", BS_PUSHBUTTON | WS_TABSTOP, rx + 276, 108, 96, 26, ID_DELSTOP);
    mk(L"STATIC", L"Drag a marker to move its color, double-click it to change the color. 2 to 5 colors.",
       SS_LEFT, rx, 140, 372, 32, 0);

    /* ---- right column: apps ---- */
    mk(L"STATIC", L"", SS_ETCHEDHORZ, rx, 180, 372, 2, 0);
    mk(L"BUTTON", L"Only react to the apps below", BS_AUTOCHECKBOX | WS_TABSTOP, rx, 190, 372, 22, ID_APPONLY);
    mk(L"LISTBOX", L"", LBS_NOTIFY | WS_VSCROLL | WS_BORDER | WS_TABSTOP, rx, 218, 372, 98, ID_APPLIST);
    mk(L"BUTTON", L"Add app...", BS_PUSHBUTTON | WS_TABSTOP, rx, 324, 130, 28, ID_APPADD);
    mk(L"BUTTON", L"Remove selected", BS_PUSHBUTTON | WS_TABSTOP, rx + 138, 324, 130, 28, ID_APPDEL);
    mk(L"STATIC", L"Per-app audio needs Windows 10 version 2004 or newer. Apps that are not running are ignored.",
       SS_LEFT, rx, 360, 372, 34, 0);

    mk(L"BUTTON", L"Reset defaults", BS_PUSHBUTTON | WS_TABSTOP, 16, 436, 120, 28, ID_RESET);
    mk(L"BUTTON", L"Test bars", BS_PUSHBUTTON | WS_TABSTOP, 146, 436, 120, 28, ID_TEST);
    mk(L"BUTTON", L"Close", BS_PUSHBUTTON | WS_TABSTOP, rx + 252, 436, 120, 28, ID_CLOSE);

    EnumChildWindows(hSet, font_proc, (LPARAM)uiFont);
    SendMessageW(hSet, WM_SETICON, ICON_BIG, (LPARAM)make_icon(32));
    SendMessageW(hSet, WM_SETICON, ICON_SMALL, (LPARAM)make_icon(GetSystemMetrics(SM_CXSMICON)));
    update_controls();
}

/* ---------------- controller window ---------------- */
static int fg_is_shell(void)
{
    static const WCHAR *shell[] = {L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd",
        L"NotifyIconOverflowWindow", L"TopLevelWindowForOverflowXamlIsland",
        L"XamlExplorerHostIslandWindow", L"Windows.UI.Core.CoreWindow"};
    HWND f = GetForegroundWindow();
    WCHAR cls[96];
    if (!f || f == hSet || f == hCtl || f == hWall) return 1;
    if (!GetClassNameW(f, cls, 96)) return 1;
    for (int i = 0; i < (int)(sizeof shell / sizeof shell[0]); i++)
        if (!lstrcmpW(cls, shell[i])) return 1;
    return 0;
}

static void do_tick(void)
{
    static int fgPause;
    static LARGE_INTEGER freq, last;
    LARGE_INTEGER now;
    if (!freq.QuadPart) { QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&last); }
    QueryPerformanceCounter(&now);
    float dt = (float)(now.QuadPart - last.QuadPart) / (float)freq.QuadPart;
    last = now;
    if (dt < 0.005f) dt = 0.005f;
    if (dt > 0.1f) dt = 0.1f;

    audio_pump(dt);
    ++tickN;
    if (tickN % S.fps == 0) {                                   /* ~once per second */
        QUERY_USER_NOTIFICATION_STATE st;
        busy = SUCCEEDED(SHQueryUserNotificationState(&st)) &&
               (st == QUNS_BUSY || st == QUNS_RUNNING_D3D_FULL_SCREEN || st == QUNS_PRESENTATION_MODE);
    }
    if (tickN % (S.fps * 2) == 0) { if (wantApps()) apps_sync(); else audio_check_device(); fix_zorder(); }
    if (!S.focusPause) fgPause = 0;
    else if (tickN % (S.fps / 6 > 0 ? S.fps / 6 : 1) == 0) fgPause = !fg_is_shell();
    if (g_log && tickN % (S.fps * 2) == 0) {
        float mx = 0;
        for (int i = 0; i < MAXBARS; i++) if (g_lvl[i] > mx) mx = g_lvl[i];
        lv_log("stats: audioOK=%d apps=%d frames=%d maxLevel=%.3f demo=%.1f fgPause=%d busy=%d paused=%d",
               audioOK, wantApps(), accN, mx, demoLeft, fgPause, busy, paused);
    }
    if (demoLeft <= 0 && (busy || fgPause)) return;             /* fullscreen app / another window focused */

    if (demoLeft > 0) {                                         /* synthetic bars: proves drawing works */
        int n = S.order == 2 ? (S.bars + 1) / 2 : S.bars;
        demoLeft -= dt; demoT += dt;
        for (int b = 0; b < n; b++) {
            float x = (float)b / n;
            float v = (0.5f + 0.5f * sinf(demoT * 5.f - b * 0.35f)) * (0.9f - 0.6f * x) + 0.1f;
            g_lvl[b] = v;
            g_peak[b] = fmaxf(v, g_peak[b] - dt * 0.6f);
        }
        for (int b = n; b < MAXBARS; b++) { g_lvl[b] = 0; g_peak[b] = 0; }
    }
    else if (paused) memset(g_lvl, 0, sizeof g_lvl);
    else analyze(&S, dt);

    int y0, y1;
    render(&S, &cv, &y0, &y1);
    if (y1 > y0) present_strip(y0, y1);
}

static void relayout_display(void)
{
    free_full();
    sw = GetSystemMetrics(SM_CXSCREEN); sh = GetSystemMetrics(SM_CYSCREEN);
    make_bg();
    load_background();
    stripH = 0;
    if (frBmp) { SelectObject(frDC, GetStockObject(NULL_BRUSH)); DeleteObject(frBmp); frBmp = NULL; }
    apply_layout();
    create_wall();
}

static LRESULT CALLBACK ctl_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == wmTaskbar) { tray_add(); create_wall(); return 0; }   /* explorer restarted */
    switch (m) {
    case WM_TIMER:
        if (w == 1) do_tick();
        else if (w == 2) { KillTimer(h, 2); settings_save(); }
        else if (w == 3) { KillTimer(h, 3); load_background(); draw_full(); cv.py0 = 0; cv.py1 = cv.H; }
        return 0;
    case WM_TRAY:
        if (l == WM_LBUTTONUP || l == WM_LBUTTONDBLCLK) {
            update_controls(); ShowWindow(hSet, SW_SHOW); SetForegroundWindow(hSet);
        } else if (l == WM_RBUTTONUP) tray_menu();
        return 0;
    case WM_SETTINGCHANGE:
        if (w == SPI_SETDESKWALLPAPER && S.useWall) SetTimer(h, 3, 700, NULL);
        return 0;
    case WM_DISPLAYCHANGE: relayout_display(); return 0;
    case WM_CLOSE:
        settings_save();
        Shell_NotifyIconW(NIM_DELETE, &nid);
        audio_close();
        for (int i = 0; i < MAXCAPS; i++) if (caps[i].ok) cap_close(&caps[i]);
        if (hWall) DestroyWindow(hWall);
        SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, NULL, SPIF_UPDATEINIFILE); /* repaint wallpaper */
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE hp, LPSTR cmd, int show)
{
    (void)hp; (void)cmd; (void)show;
    hInst = hi;
    HANDLE mtx = CreateMutexW(NULL, TRUE, L"LiteVisSingleInstance2");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    for (int i = 1; i < __argc; i++)
        if (!strcmp(__argv[i], "--startup")) Sleep(3000);       /* let the shell finish loading */
    for (int i = 1; i < __argc; i++)
        if (!strcmp(__argv[i], "--log")) g_log = 1;

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);
    HDC scr = GetDC(NULL);
    dpi = GetDeviceCaps(scr, LOGPIXELSX);
    bgDC = CreateCompatibleDC(scr); frDC = CreateCompatibleDC(scr);
    ReleaseDC(NULL, scr);

    settings_load();
    audio_apply_mode();                                         /* retried automatically if it fails */

    WNDCLASSW wc = {0};
    wc.hInstance = hi;
    wc.lpfnWndProc = wall_proc; wc.lpszClassName = L"LiteVisWall"; RegisterClassW(&wc);
    wc.lpfnWndProc = ctl_proc;  wc.lpszClassName = L"LiteVisCtl";  RegisterClassW(&wc);
    wc.lpfnWndProc = ge_proc;   wc.lpszClassName = L"GradEditor";
    wc.style = CS_DBLCLKS; wc.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512)); RegisterClassW(&wc);
    wc.style = 0;
    wc.lpfnWndProc = set_proc;  wc.lpszClassName = L"LiteVisSet";
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512));
    RegisterClassW(&wc);
    wmTaskbar = RegisterWindowMessageW(L"TaskbarCreated");
    pSetThreadDpiCtx = (DPICTX (WINAPI *)(DPICTX))(void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext");
    lv_log("--- Latte Visualizer start (build 7) ---");

    hCtl = CreateWindowExW(0, L"LiteVisCtl", L"LiteVisCtl", WS_POPUP, 0, 0, 0, 0, NULL, NULL, hi, NULL);

    sw = GetSystemMetrics(SM_CXSCREEN); sh = GetSystemMetrics(SM_CYSCREEN);
    make_bg();
    load_background();
    apply_layout();
    create_wall();
    create_settings();
    tray_add();
    SetTimer(hCtl, 1, 1000 / S.fps, NULL);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (hSet && IsDialogMessageW(hSet, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    ReleaseMutex(mtx);
    return 0;
}

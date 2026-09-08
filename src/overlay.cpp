#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

// === MUST BE FIRST ===
#include <windows.h>
#include <commdlg.h>
#pragma comment(lib, "comdlg32.lib")

#include <windowsx.h>
#include <dwmapi.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <vector>
#include <filesystem>
#include <chrono>
#include <iomanip>
#include <sstream>

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")

#define PIPE_NAME_W     L"\\\\.\\pipe\\fuckoffmaxey"
#define FLAG_COLOR_OK   (1u << 0)
#define FLAG_DEPTH_OK   (1u << 1)
#define FLAG_FPS_OK     (1u << 2)
#define FLAG_VIEWPORT_OK (1u << 4)
#define PAYLOAD_SIZE    0x40u

#pragma pack(push, 1)
struct PipePayloadRaw40
{
    uint64_t depthHandle;
    uint64_t colorHandle;
    uint32_t depthWidth;
    uint32_t depthHeight;
    uint32_t colorWidth;
    uint32_t colorHeight;
    uint32_t depthFormat;
    uint32_t colorFormat;
    uint32_t frameCount;
    uint32_t flags;
    uint32_t vpX;
    uint32_t vpY;
    uint32_t vpWidth;
    uint32_t vpHeight;
};
#pragma pack(pop)
static_assert(sizeof(PipePayloadRaw40) == PAYLOAD_SIZE, "PipePayloadRaw40 must be 0x40 bytes");

struct PipePayload
{
    uint64_t depthHandle = 0;
    uint64_t colorHandle = 0;
    uint32_t depthWidth = 0;
    uint32_t depthHeight = 0;
    uint32_t colorWidth = 0;
    uint32_t colorHeight = 0;
    uint32_t depthFormat = 0;
    uint32_t colorFormat = 0;
    uint32_t frameCount = 0;
    uint32_t flags = 0;
    uint32_t vpX = 0;
    uint32_t vpY = 0;
    uint32_t vpWidth = 0;
    uint32_t vpHeight = 0;
};

template <typename T>
static void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

// ---------------------------------------------------------------------------
// Format helpers
// ---------------------------------------------------------------------------
static DXGI_FORMAT DepthSRVFormat(uint32_t fmt)
{
    DXGI_FORMAT f = (DXGI_FORMAT)fmt;
    switch (f)
    {
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_UNORM:
        return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    default:
        return DXGI_FORMAT_R32_FLOAT;
    }
}

static DXGI_FORMAT ColorSRVFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:   return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:   return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:   return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8X8_UNORM;
    default: return f;
    }
}

static PipePayload NormalizePayload(const uint8_t raw[PAYLOAD_SIZE])
{
    const PipePayloadRaw40* r = reinterpret_cast<const PipePayloadRaw40*>(raw);
    PipePayload p;
    p.depthHandle = r->depthHandle;
    p.colorHandle = r->colorHandle;
    p.depthWidth = r->depthWidth ? r->depthWidth : r->colorWidth;
    p.depthHeight = r->depthHeight ? r->depthHeight : r->colorHeight;
    p.colorWidth = r->colorWidth;
    p.colorHeight = r->colorHeight;
    p.depthFormat = r->depthFormat;
    p.colorFormat = r->colorFormat;
    p.frameCount = r->frameCount;
    p.flags = r->flags;
    p.vpX = r->vpX;
    p.vpY = r->vpY;
    p.vpWidth = r->vpWidth;
    p.vpHeight = r->vpHeight;

    if ((p.flags & FLAG_VIEWPORT_OK) && p.vpWidth && p.vpHeight)
    {
        if (!p.colorWidth)  p.colorWidth = p.vpWidth;
        if (!p.colorHeight) p.colorHeight = p.vpHeight;
        if (!p.depthWidth)  p.depthWidth = p.vpWidth;
        if (!p.depthHeight) p.depthHeight = p.vpHeight;
    }
    return p;
}

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
static HWND   g_hwnd = nullptr;
static HWND   g_target = nullptr;
static DWORD  g_targetPid = 0;
static bool   g_console = false;
static bool   g_desktop = false;
static bool   g_running = true;
static bool   g_menu = false;
static bool   g_reshadeInput = false;
static bool   g_transparent = false;
static bool   g_overlayEnabled = true;
static bool   g_overlayHiddenForForeground = false;
static bool   g_vsync = true;
static bool   g_lowPerf = false;
static UINT   g_width = 1280, g_height = 720;
static DWORD  g_lastFollow = 0;
static RECT   g_lastRect = { 0,0,0,0 };

// --- depth semantics (single source of truth, mirrored into ReShade.ini) ----
static bool  g_depthReversed = true;   // Roblox = reversed-Z (near = 1, far = 0)
static bool  g_depthUpsideDown = false;
static bool  g_depthLogarithmic = false;
static float g_depthFarPlane = 1000.0f;
static bool  g_depthPreview = false;   // F3: on-screen depth debug view
static bool  g_useViewportRemap = true;// use vp* rect from payload
static bool  g_depthPassEnabled = true;

static ID3D11Device* g_dev = nullptr;
static ID3D11DeviceContext* g_ctx = nullptr;
static IDXGISwapChain* g_swap = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;
static ID3D11Texture2D* g_localDepth = nullptr;
static ID3D11DepthStencilView* g_localDSV = nullptr;
static ID3D11ShaderResourceView* g_localDepthSRV = nullptr;
static ID3D11DepthStencilState* g_dsWrite = nullptr;
static ID3D11DepthStencilState* g_dsOff = nullptr;
static ID3D11SamplerState* g_sampLinear = nullptr;
static ID3D11SamplerState* g_sampPoint = nullptr;
static ID3D11Buffer* g_cb = nullptr;
static ID3D11VertexShader* g_vs = nullptr;
static ID3D11PixelShader* g_psDepth = nullptr;
static ID3D11PixelShader* g_psColor = nullptr;
static ID3D11PixelShader* g_psDepthView = nullptr;

static ID3D11Texture2D* g_depthTex = nullptr;
static ID3D11ShaderResourceView* g_depthSRV = nullptr;
static ID3D11Texture2D* g_colorTex = nullptr;
static ID3D11ShaderResourceView* g_colorSRV = nullptr;
static uint64_t g_lastDepthHandle = 0, g_lastColorHandle = 0;
static uint32_t g_lastDepthW = 0, g_lastDepthH = 0, g_lastDepthFmt = 0;
static uint32_t g_lastColorW = 0, g_lastColorH = 0, g_lastColorFmt = 0;
static HRESULT  g_lastDepthOpenHR = S_OK, g_lastColorOpenHR = S_OK;
static HRESULT  g_lastDepthSRVHR = S_OK, g_lastColorSRVHR = S_OK;
static UINT     g_depthTexW = 0, g_depthTexH = 0;  // real opened texture size
static UINT     g_colorTexW = 0, g_colorTexH = 0;
static int      g_depthHeuristicDraws = 4;
static bool     g_showFPS = true;
static float    g_fpsColor[4] = { 1.0f, 0.95f, 0.70f, 1.0f };
static int  g_minimizeKey = VK_F1;

static int  g_waitKeyTarget = 0;
static DWORD g_waitKeyStart = 0;
static float g_fps = 0.0f;
static uint32_t g_prevFrameCount = 0;
static std::string g_gpuName = "Unknown GPU";
static DWORD g_resyncDueTick = 0;

static std::thread       g_pipeThread;
static std::mutex        g_pipeMutex;
static std::atomic<bool> g_pipeStop{ false };
static std::atomic<bool> g_pipeConnected{ false };
static std::atomic<bool> g_hasPayload{ false };
static PipePayload       g_payload;

static char g_iniPath[MAX_PATH] = {};
static char g_reshadeIniPath[MAX_PATH] = {};

static void SetMenu(bool v);
static void RequestOverlayResync(const char* reason, DWORD delayMs);
static void ApplyWindowMode();
static void FocusRoblox();
static void SaveSettings();
static void WriteReShadeConfig();

static void Log(const char* fmt, ...)
{
    char b[2048];
    va_list va; va_start(va, fmt); vsnprintf(b, sizeof(b), fmt, va); va_end(va);
    OutputDebugStringA(b); OutputDebugStringA("\n");
    if (g_console) { printf("%s\n", b); fflush(stdout); }
}
static bool HasArg(const char* a) { const char* c = GetCommandLineA(); return c && strstr(c, a); }

static void QueryGpuNameFromDevice()
{
    if (!g_dev) return;
    IDXGIDevice* dxgiDevice = nullptr;
    if (FAILED(g_dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice)) || !dxgiDevice)
        return;
    IDXGIAdapter* adapter = nullptr;
    if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) && adapter)
    {
        DXGI_ADAPTER_DESC desc = {};
        if (SUCCEEDED(adapter->GetDesc(&desc)))
        {
            char name[256] = {};
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);
            if (name[0]) g_gpuName = name;
        }
        adapter->Release();
    }
    dxgiDevice->Release();
}

static void UpdateFPS(uint32_t currentFrameCount)
{
    static auto s_lastTime = std::chrono::high_resolution_clock::now();
    static float s_smoothedFPS = 0.0f;

    auto now = std::chrono::high_resolution_clock::now();
    float dt = std::chrono::duration<float>(now - s_lastTime).count();
    s_lastTime = now;

    if (currentFrameCount != g_prevFrameCount)
    {
        g_prevFrameCount = currentFrameCount;
        if (dt > 0.0001f && dt < 1.0f)
        {
            float instantFPS = 1.0f / dt;
            if (s_smoothedFPS <= 0.0f) s_smoothedFPS = instantFPS;
            else                       s_smoothedFPS = s_smoothedFPS * 0.95f + instantFPS * 0.05f;
            g_fps = s_smoothedFPS;
        }
    }
}

static const char* VkKeyName(int vk)
{
    static char name[64];
    name[0] = 0;
    UINT scan = MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_VSC);
    LONG lp = (LONG)(scan << 16);
    switch (vk)
    {
    case VK_INSERT: case VK_DELETE: case VK_END:
    case VK_PRIOR:  case VK_NEXT:  case VK_HOME:
    case VK_LEFT:   case VK_RIGHT: case VK_UP: case VK_DOWN:
        lp |= (1 << 24);
        break;
    default: break;
    }
    if (GetKeyNameTextA(lp, name, sizeof(name)) > 0)
        return name;
    snprintf(name, sizeof(name), "VK_%02X", vk & 0xff);
    return name;
}

// ===========================================================================
//  Minimal order-preserving INI reader/writer (replaces the old broken
//  string-splicing code that could duplicate the [INPUT] section).
// ===========================================================================
struct IniSection
{
    std::string name;
    std::vector<std::pair<std::string, std::string>> kv;
};

static std::string TrimStr(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static std::vector<IniSection> IniLoad(const char* path)
{
    std::vector<IniSection> secs;
    FILE* f = fopen(path, "rb");
    if (!f) return secs;
    char line[4096];
    IniSection* cur = nullptr;
    while (fgets(line, sizeof(line), f))
    {
        std::string s = TrimStr(line);
        if (s.empty() || s[0] == ';' || s[0] == '#') continue;
        if (s.front() == '[' && s.back() == ']')
        {
            secs.push_back(IniSection{ s.substr(1, s.size() - 2), {} });
            cur = &secs.back();
            continue;
        }
        size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        if (!cur) { secs.push_back(IniSection{ "", {} }); cur = &secs.back(); }
        cur->kv.emplace_back(TrimStr(s.substr(0, eq)), TrimStr(s.substr(eq + 1)));
    }
    fclose(f);
    return secs;
}

static IniSection& IniSec(std::vector<IniSection>& secs, const char* name)
{
    for (auto& s : secs)
        if (_stricmp(s.name.c_str(), name) == 0)
            return s;
    secs.push_back(IniSection{ name, {} });
    return secs.back();
}

static const char* IniGet(std::vector<IniSection>& secs, const char* sec, const char* key, const char* def = "")
{
    for (auto& s : secs)
        if (_stricmp(s.name.c_str(), sec) == 0)
            for (auto& p : s.kv)
                if (_stricmp(p.first.c_str(), key) == 0)
                    return p.second.c_str();
    return def;
}

static void IniSet(std::vector<IniSection>& secs, const char* sec, const char* key, const std::string& val)
{
    IniSection& s = IniSec(secs, sec);
    for (auto& p : s.kv)
        if (_stricmp(p.first.c_str(), key) == 0) { p.second = val; return; }
    s.kv.emplace_back(key, val);
}

static bool IniSave(const char* path, const std::vector<IniSection>& secs)
{
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    for (const auto& s : secs)
    {
        if (!s.name.empty()) fprintf(f, "[%s]\n", s.name.c_str());
        for (const auto& p : s.kv)
            fprintf(f, "%s=%s\n", p.first.c_str(), p.second.c_str());
        fprintf(f, "\n");
    }
    fclose(f);
    return true;
}

// Merge a single RESHADE_* define into an existing comma separated list,
// replacing any previous value instead of appending a duplicate.
static void MergeDefine(std::vector<std::string>& defs, const std::string& key, const std::string& value)
{
    for (auto& d : defs)
    {
        size_t eq = d.find('=');
        std::string k = (eq == std::string::npos) ? d : d.substr(0, eq);
        if (_stricmp(TrimStr(k).c_str(), key.c_str()) == 0)
        {
            d = key + "=" + value;
            return;
        }
    }
    defs.push_back(key + "=" + value);
}

// ===========================================================================
//  ReShade.ini: depth-buffer configuration.
//
//  This is the part that actually decides whether SSR / MXAO / RTGI work.
//  The old code only wrote KeyOverlay and never told ReShade how to interpret
//  our depth buffer, so every depth-based effect (SSR first of all) silently
//  produced garbage.
// ===========================================================================
static void WriteReShadeConfig()
{
    if (!g_reshadeIniPath[0]) return;

    std::vector<IniSection> ini = IniLoad(g_reshadeIniPath);

    // ---- overlay key (Home) -------------------------------------------------
    IniSet(ini, "INPUT", "KeyOverlay", "36,0,0,0");

    // ---- generic depth add-on ----------------------------------------------
    // DepthCopyBeforeClears MUST be 0: we fill the depth buffer once per frame
    // and never clear it afterwards, so "as-is at end of frame" is correct.
    // Any other value makes ReShade grab the buffer *before* our blit -> the
    // depth it hands to SSR is an empty / one-frame-stale buffer.
    IniSet(ini, "DEPTH", "DepthCopyBeforeClears", "0");
    IniSet(ini, "DEPTH", "DepthCopyAtClearIndex", "0");
    IniSet(ini, "DEPTH", "DisableINTZ", "0");
    // 2 = matching aspect ratio but relaxed (covers resolution scaling / DLSS,
    // and the Roblox window-vs-render-target size mismatch).
    IniSet(ini, "DEPTH", "UseAspectRatioHeuristics", "2");
    IniSet(ini, "DEPTH", "DrawStatsHeuristic", "0");

    // ---- preprocessor definitions ------------------------------------------
    // Keep every user define, only force the four depth ones so that the
    // shader-side interpretation can never disagree with what we upload.
    std::string cur = IniGet(ini, "GENERAL", "PreprocessorDefinitions", "");
    std::vector<std::string> defs;
    {
        size_t start = 0;
        while (start <= cur.size())
        {
            size_t c = cur.find(',', start);
            std::string item = TrimStr(cur.substr(start, c == std::string::npos ? std::string::npos : c - start));
            if (!item.empty()) defs.push_back(item);
            if (c == std::string::npos) break;
            start = c + 1;
        }
    }

    char farbuf[64];
    snprintf(farbuf, sizeof(farbuf), "%.1f", g_depthFarPlane);
    MergeDefine(defs, "RESHADE_DEPTH_LINEARIZATION_FAR_PLANE", farbuf);
    MergeDefine(defs, "RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN", g_depthUpsideDown ? "1" : "0");
    MergeDefine(defs, "RESHADE_DEPTH_INPUT_IS_REVERSED", g_depthReversed ? "1" : "0");
    MergeDefine(defs, "RESHADE_DEPTH_INPUT_IS_LOGARITHMIC", g_depthLogarithmic ? "1" : "0");

    std::string joined;
    for (size_t i = 0; i < defs.size(); ++i)
    {
        if (i) joined += ",";
        joined += defs[i];
    }
    IniSet(ini, "GENERAL", "PreprocessorDefinitions", joined);

    if (IniSave(g_reshadeIniPath, ini))
        Log("[ReShade] config written (%s), reversed=%d upsideDown=%d log=%d",
            g_reshadeIniPath, (int)g_depthReversed, (int)g_depthUpsideDown, (int)g_depthLogarithmic);
    else
        Log("[ReShade] cannot write %s", g_reshadeIniPath);
}

// ===========================================================================
//  Overlay settings
// ===========================================================================
static void LoadSettings()
{
    if (!g_iniPath[0]) return;
    std::vector<IniSection> ini = IniLoad(g_iniPath);
    if (ini.empty()) return;

    int v = atoi(IniGet(ini, "Keybinds", "Minimize", "0"));
    if (v > 0 && v < 256) g_minimizeKey = v;

    g_showFPS = atoi(IniGet(ini, "Style", "ShowFPS", "1")) != 0;
    {
        float r = 0, g = 0, b = 0, a = 0;
        if (sscanf(IniGet(ini, "Style", "FpsColor", ""), "%f,%f,%f,%f", &r, &g, &b, &a) == 4)
        {
            g_fpsColor[0] = std::max(0.0f, std::min(1.0f, r));
            g_fpsColor[1] = std::max(0.0f, std::min(1.0f, g));
            g_fpsColor[2] = std::max(0.0f, std::min(1.0f, b));
            g_fpsColor[3] = std::max(0.0f, std::min(1.0f, a));
        }
    }

    g_depthReversed = atoi(IniGet(ini, "Depth", "Reversed", "1")) != 0;
    g_depthUpsideDown = atoi(IniGet(ini, "Depth", "UpsideDown", "0")) != 0;
    g_depthLogarithmic = atoi(IniGet(ini, "Depth", "Logarithmic", "0")) != 0;
    g_useViewportRemap = atoi(IniGet(ini, "Depth", "ViewportRemap", "1")) != 0;
    {
        float fp = (float)atof(IniGet(ini, "Depth", "FarPlane", "1000.0"));
        if (fp > 1.0f) g_depthFarPlane = fp;
    }
}

static void SaveSettings()
{
    if (!g_iniPath[0]) return;
    std::vector<IniSection> ini;
    IniSet(ini, "Keybinds", "Minimize", std::to_string(g_minimizeKey));
    IniSet(ini, "Style", "ShowFPS", g_showFPS ? "1" : "0");
    {
        char buf[128];
        snprintf(buf, sizeof(buf), "%.6f,%.6f,%.6f,%.6f",
            g_fpsColor[0], g_fpsColor[1], g_fpsColor[2], g_fpsColor[3]);
        IniSet(ini, "Style", "FpsColor", buf);
    }
    IniSet(ini, "Depth", "Reversed", g_depthReversed ? "1" : "0");
    IniSet(ini, "Depth", "UpsideDown", g_depthUpsideDown ? "1" : "0");
    IniSet(ini, "Depth", "Logarithmic", g_depthLogarithmic ? "1" : "0");
    IniSet(ini, "Depth", "ViewportRemap", g_useViewportRemap ? "1" : "0");
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.1f", g_depthFarPlane);
        IniSet(ini, "Depth", "FarPlane", buf);
    }
    IniSave(g_iniPath, ini);

    WriteReShadeConfig();
}

static bool CaptureKeyIfWaiting()
{
    if (!g_waitKeyTarget) return false;
    if (GetTickCount() - g_waitKeyStart < 250) return true;

    for (int vk = 8; vk < 256; ++vk)
    {
        if (!(GetAsyncKeyState(vk) & 1)) continue;
        if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT ||
            vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL ||
            vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU ||
            vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON ||
            vk == VK_XBUTTON1 || vk == VK_XBUTTON2)
            continue;
        if (vk == VK_ESCAPE) { g_waitKeyTarget = 0; return true; }
        if (g_waitKeyTarget == 1) g_minimizeKey = vk;
        g_waitKeyTarget = 0;
        SaveSettings();
        return true;
    }
    return true;
}

// ===========================================================================
//  Pipe
// ===========================================================================
static bool ReadExact(HANDLE h, void* out, DWORD sz)
{
    BYTE* p = (BYTE*)out; DWORD total = 0;
    while (total < sz && !g_pipeStop.load())
    {
        DWORD br = 0;
        if (!ReadFile(h, p + total, sz - total, &br, nullptr) || br == 0) return false;
        total += br;
    }
    return total == sz;
}

static void PipeThread()
{
    Log("[Pipe] waiting for %ls", PIPE_NAME_W);
    while (!g_pipeStop.load())
    {
        HANDLE h = INVALID_HANDLE_VALUE;
        while (h == INVALID_HANDLE_VALUE && !g_pipeStop.load())
        {
            h = CreateFileW(PIPE_NAME_W, GENERIC_READ, 0, nullptr,
                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE)
            {
                if (GetLastError() == ERROR_PIPE_BUSY) WaitNamedPipeW(PIPE_NAME_W, 500);
                else Sleep(300);
            }
        }
        if (g_pipeStop.load()) { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); break; }
        g_pipeConnected.store(true);
        Log("[Pipe] connected");
        while (!g_pipeStop.load())
        {
            uint8_t raw[PAYLOAD_SIZE] = {};
            if (!ReadExact(h, raw, sizeof(raw))) break;
            PipePayload p = NormalizePayload(raw);
            if (p.depthHandle || p.colorHandle)
            {
                std::lock_guard<std::mutex> lk(g_pipeMutex);
                g_payload = p;
                g_hasPayload.store(true);
            }
        }
        CloseHandle(h);
        g_pipeConnected.store(false);
        Log("[Pipe] disconnected");
        Sleep(300);
    }
}

// ===========================================================================
//  Shaders
//
//  cb0 layout (32 bytes):
//    float4 gUV;    // xy = uv scale, zw = uv bias  (viewport remap)
//    float4 gMisc;  // x = invert depth, y = far value, zw = unused
// ===========================================================================
struct RemapCB
{
    float uvScale[2];
    float uvBias[2];
    float misc[4];
};
static_assert(sizeof(RemapCB) == 32, "cb size");

static const char* VS_SRC = R"HLSL(
struct O { float4 p:SV_POSITION; float2 uv:TEXCOORD0; };
O main(uint id:SV_VertexID){
    O o;
    float2 uv = float2((id<<1)&2, id&2);
    o.uv = uv;
    o.p  = float4(uv*float2(2,-2)+float2(-1,1),0,1);
    return o;
}
)HLSL";

static const char* PS_COLOR = R"HLSL(
cbuffer P : register(b0) { float4 gUV; float4 gMisc; };
Texture2D<float4> T:register(t0);
SamplerState S:register(s0);
float4 main(float4 p:SV_POSITION, float2 uv:TEXCOORD0):SV_Target {
    float2 c = uv * gUV.xy + gUV.zw;
    float4 col = T.SampleLevel(S, c, 0);
    return float4(col.rgb, 1);
}
)HLSL";

// Raw pass-through of the game depth. We deliberately do NOT normalise or
// invert here: reversed-Z carries almost all of its precision near 1.0, and
// converting it to "classic" 0..1 depth destroys the far-field precision that
// SSR ray-marching depends on. ReShade is told about the convention through
// RESHADE_DEPTH_INPUT_IS_REVERSED instead.
static const char* PS_DEPTH = R"HLSL(
cbuffer P : register(b0) { float4 gUV; float4 gMisc; };
Texture2D<float> T:register(t0);
SamplerState S:register(s0);
float main(float4 p:SV_POSITION, float2 uv:TEXCOORD0):SV_Depth {
    float2 c = uv * gUV.xy + gUV.zw;
    float d = T.SampleLevel(S, c, 0).r;
    if (gMisc.x > 0.5) d = 1.0 - d;
    return saturate(d);
}
)HLSL";

static const char* PS_DEPTH_VIEW = R"HLSL(
cbuffer P : register(b0) { float4 gUV; float4 gMisc; };
Texture2D<float> T:register(t0);
SamplerState S:register(s0);
float4 main(float4 p:SV_POSITION, float2 uv:TEXCOORD0):SV_Target {
    float2 c = uv * gUV.xy + gUV.zw;
    float d = T.SampleLevel(S, c, 0).r;
    if (gMisc.x > 0.5) d = 1.0 - d;
    // linearise for a readable preview
    float f = max(gMisc.y, 1.0);
    float lin = 1.0 / (d * (1.0 - 1.0 / f) + (1.0 / f));
    lin = saturate(lin / f);
    return float4(lin, lin, lin, 1);
}
)HLSL";

static bool Compile(const char* src, const char* target, ID3DBlob** out)
{
    ID3DBlob* e = nullptr;
    HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr,
        "main", target, D3DCOMPILE_ENABLE_STRICTNESS, 0, out, &e);
    if (FAILED(hr))
    {
        Log("[Shader] %s failed: %s", target, e ? (char*)e->GetBufferPointer() : "?");
        if (e) e->Release();
        return false;
    }
    if (e) e->Release();
    return true;
}

// The buffer ReShade's generic-depth add-on will pick up.
// Created as R32_TYPELESS + DEPTH_STENCIL|SHADER_RESOURCE, exactly the layout
// a real game (and the INTZ path ReShade expects) produces. A plain
// D32_FLOAT / depth-stencil-only texture is far more likely to be skipped or
// mis-copied by the add-on.
static bool CreateLocalDepth(UINT w, UINT h)
{
    SafeRelease(g_localDepthSRV);
    SafeRelease(g_localDSV);
    SafeRelease(g_localDepth);

    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R32_TYPELESS;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &g_localDepth)))
    {
        // fall back to the plain format if the driver refuses the typeless one
        d.Format = DXGI_FORMAT_D32_FLOAT;
        d.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &g_localDepth)))
            return false;
    }

    D3D11_DEPTH_STENCIL_VIEW_DESC v = {};
    v.Format = DXGI_FORMAT_D32_FLOAT;
    v.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    if (FAILED(g_dev->CreateDepthStencilView(g_localDepth, &v, &g_localDSV)))
        return false;

    if (d.BindFlags & D3D11_BIND_SHADER_RESOURCE)
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC s = {};
        s.Format = DXGI_FORMAT_R32_FLOAT;
        s.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        s.Texture2D.MipLevels = 1;
        g_dev->CreateShaderResourceView(g_localDepth, &s, &g_localDepthSRV);
    }
    return true;
}

static bool Resize(UINT w, UINT h)
{
    if (!w || !h || (w == g_width && h == g_height)) return true;
    g_width = w; g_height = h;
    g_ctx->OMSetRenderTargets(0, nullptr, nullptr);
    SafeRelease(g_rtv); SafeRelease(g_localDepthSRV); SafeRelease(g_localDSV); SafeRelease(g_localDepth);
    HRESULT hr = g_swap->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) { Log("[D3D] ResizeBuffers failed 0x%08lX", (unsigned long)hr); return false; }
    ID3D11Texture2D* bb = nullptr;
    hr = g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb);
    if (FAILED(hr) || !bb) return false;
    g_dev->CreateRenderTargetView(bb, nullptr, &g_rtv);
    bb->Release();
    CreateLocalDepth(w, h);
    Log("[D3D] resized to %ux%u", w, h);
    return true;
}

static bool OpenSharedTex(uint64_t handleValue, ID3D11Texture2D** out, HRESULT* outHr)
{
    *out = nullptr;
    HANDLE h = (HANDLE)(uintptr_t)handleValue;
    HRESULT hr = g_dev->OpenSharedResource(h, __uuidof(ID3D11Texture2D), (void**)out);
    if (outHr) *outHr = hr;
    return SUCCEEDED(hr) && *out;
}

// Effective presentation size = the viewport rectangle if the layer reported
// one, otherwise the full colour texture.
static void EffectiveSize(const PipePayload& p, UINT& outW, UINT& outH)
{
    if (g_useViewportRemap && (p.flags & FLAG_VIEWPORT_OK) && p.vpWidth && p.vpHeight)
    {
        outW = p.vpWidth;
        outH = p.vpHeight;
        return;
    }
    outW = p.colorWidth ? p.colorWidth : p.depthWidth;
    outH = p.colorHeight ? p.colorHeight : p.depthHeight;
}

static void UpdateResources(const PipePayload& p)
{
    UINT ew = 0, eh = 0;
    EffectiveSize(p, ew, eh);

    if (p.colorHandle && p.colorWidth && p.colorHeight)
    {
        bool ch = p.colorHandle != g_lastColorHandle ||
            p.colorWidth != g_lastColorW ||
            p.colorHeight != g_lastColorH ||
            p.colorFormat != g_lastColorFmt;
        if (ch || !g_colorSRV)
        {
            SafeRelease(g_colorSRV); SafeRelease(g_colorTex);
            g_colorTexW = g_colorTexH = 0;
            if (OpenSharedTex(p.colorHandle, &g_colorTex, &g_lastColorOpenHR))
            {
                D3D11_TEXTURE2D_DESC td = {};
                g_colorTex->GetDesc(&td);
                g_colorTexW = td.Width; g_colorTexH = td.Height;
                D3D11_SHADER_RESOURCE_VIEW_DESC s = {};
                s.Format = ColorSRVFormat(td.Format);
                s.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                s.Texture2D.MipLevels = 1;
                g_lastColorSRVHR = g_dev->CreateShaderResourceView(g_colorTex, &s, &g_colorSRV);
                Log("[Color] tex %ux%u fmt=%u srvHR=0x%08lX",
                    td.Width, td.Height, td.Format, (unsigned long)g_lastColorSRVHR);
            }
            else Log("[Color] Open failed 0x%08lX", (unsigned long)g_lastColorOpenHR);
            g_lastColorHandle = p.colorHandle;
            g_lastColorW = p.colorWidth; g_lastColorH = p.colorHeight;
            g_lastColorFmt = p.colorFormat;
        }
    }

    if (p.depthHandle && (p.depthWidth || p.colorWidth) && (p.depthHeight || p.colorHeight))
    {
        uint32_t dw = p.depthWidth ? p.depthWidth : p.colorWidth;
        uint32_t dh = p.depthHeight ? p.depthHeight : p.colorHeight;
        bool ch = p.depthHandle != g_lastDepthHandle ||
            dw != g_lastDepthW || dh != g_lastDepthH ||
            p.depthFormat != g_lastDepthFmt;
        if (ch || !g_depthSRV)
        {
            SafeRelease(g_depthSRV); SafeRelease(g_depthTex);
            g_depthTexW = g_depthTexH = 0;
            if (OpenSharedTex(p.depthHandle, &g_depthTex, &g_lastDepthOpenHR))
            {
                D3D11_TEXTURE2D_DESC td = {};
                g_depthTex->GetDesc(&td);
                g_depthTexW = td.Width; g_depthTexH = td.Height;
                D3D11_SHADER_RESOURCE_VIEW_DESC s = {};
                s.Format = DepthSRVFormat(p.depthFormat ? p.depthFormat : td.Format);
                s.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                s.Texture2D.MipLevels = 1;
                g_lastDepthSRVHR = g_dev->CreateShaderResourceView(g_depthTex, &s, &g_depthSRV);
                Log("[Depth] tex %ux%u payloadFmt=%u texFmt=%u srvFmt=%u srvHR=0x%08lX",
                    td.Width, td.Height, p.depthFormat, td.Format, s.Format,
                    (unsigned long)g_lastDepthSRVHR);
            }
            else Log("[Depth] Open failed 0x%08lX", (unsigned long)g_lastDepthOpenHR);
            g_lastDepthHandle = p.depthHandle;
            g_lastDepthW = dw; g_lastDepthH = dh;
            g_lastDepthFmt = p.depthFormat;
        }
    }

    // Swap chain follows the *viewport*, not the raw shared texture, so that
    // colour and depth stay pixel-aligned and ReShade's aspect-ratio heuristic
    // sees a depth buffer that matches the presentation size.
    if (ew && eh) Resize(ew, eh);
}

static void SetRemap(UINT texW, UINT texH, const PipePayload& p, bool invert)
{
    RemapCB cb = {};
    cb.uvScale[0] = 1.0f; cb.uvScale[1] = 1.0f;
    cb.uvBias[0] = 0.0f; cb.uvBias[1] = 0.0f;

    if (g_useViewportRemap && (p.flags & FLAG_VIEWPORT_OK) &&
        p.vpWidth && p.vpHeight && texW && texH)
    {
        cb.uvScale[0] = (float)p.vpWidth / (float)texW;
        cb.uvScale[1] = (float)p.vpHeight / (float)texH;
        cb.uvBias[0] = (float)p.vpX / (float)texW;
        cb.uvBias[1] = (float)p.vpY / (float)texH;
    }

    if (g_depthUpsideDown && invert == false) { /* colour is never flipped */ }

    cb.misc[0] = 0.0f;                 // shader-side invert: off (see PS_DEPTH note)
    cb.misc[1] = g_depthFarPlane;
    cb.misc[2] = 0.0f;
    cb.misc[3] = 0.0f;

    D3D11_MAPPED_SUBRESOURCE m = {};
    if (SUCCEEDED(g_ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
    {
        memcpy(m.pData, &cb, sizeof(cb));
        g_ctx->Unmap(g_cb, 0);
    }
    g_ctx->PSSetConstantBuffers(0, 1, &g_cb);
}

static void DrawTri()
{
    g_ctx->IASetInputLayout(nullptr);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
    g_ctx->Draw(3, 0);
}

static void ViewportFull()
{
    D3D11_VIEWPORT v = {};
    v.Width = (float)g_width;
    v.Height = (float)g_height;
    v.MinDepth = 0; v.MaxDepth = 1;
    g_ctx->RSSetViewports(1, &v);
}

static const char* OverlayStatus(const PipePayload& p)
{
    static char text[192];
    if (!g_pipeConnected.load())   return "Searching pipe";
    if (!g_hasPayload.load())      return "Pipe connected, waiting payload";
    bool colorReady = g_colorSRV && p.colorHandle;
    bool depthReady = g_depthSRV && p.depthHandle;
    if (colorReady && depthReady)  return "Connected: color + depth";
    if (colorReady)                return "Connected: color only";
    if (depthReady)                return "Connected: depth only";
    if (p.colorHandle || p.depthHandle)
    {
        snprintf(text, sizeof(text),
            "Connected: opening C=0x%08lX D=0x%08lX",
            (unsigned long)g_lastColorOpenHR, (unsigned long)g_lastDepthOpenHR);
        return text;
    }
    return "Connected: no handles";
}

static void Render(const PipePayload& p)
{
    UpdateResources(p);

    float clear[4] = { 0,0,0, g_transparent ? 0.0f : 1.0f };
    g_ctx->ClearRenderTargetView(g_rtv, clear);
    ViewportFull();

    // ================= DEPTH PASS =================
    // Runs every frame, unconditionally. If the shared depth is missing we
    // still clear to the far value, otherwise ReShade keeps consuming a frozen
    // buffer from the last good frame (a classic "SSR smears / sticks" bug).
    const float farValue = g_depthReversed ? 0.0f : 1.0f;

    if (g_localDSV)
    {
        g_ctx->ClearDepthStencilView(g_localDSV, D3D11_CLEAR_DEPTH, farValue, 0);

        if (g_depthPassEnabled && g_depthSRV)
        {
            g_ctx->OMSetRenderTargets(1, &g_rtv, g_localDSV);
            g_ctx->OMSetDepthStencilState(g_dsWrite, 0);
            g_ctx->VSSetShader(g_vs, nullptr, 0);
            g_ctx->PSSetShader(g_psDepth, nullptr, 0);
            SetRemap(g_depthTexW, g_depthTexH, p, true);
            g_ctx->PSSetShaderResources(0, 1, &g_depthSRV);
            // POINT sampling: linear filtering interpolates across depth
            // discontinuities and invents surfaces that never existed, which
            // is exactly what makes SSR produce halos and wrong hits.
            g_ctx->PSSetSamplers(0, 1, &g_sampPoint);
            DrawTri();

            // Draw-stat padding for ReShade's generic-depth heuristic.
            // Depth writes are disabled here so these dummy draws can no
            // longer corrupt the top-left pixel of the real depth buffer.
            if (g_depthHeuristicDraws > 0)
            {
                g_ctx->OMSetDepthStencilState(g_dsOff, 0);
                D3D11_VIEWPORT tiny = {};
                tiny.Width = 1; tiny.Height = 1; tiny.MaxDepth = 1;
                g_ctx->RSSetViewports(1, &tiny);
                for (int i = 0; i < g_depthHeuristicDraws; i++) DrawTri();
                ViewportFull();
            }

            ID3D11ShaderResourceView* n = nullptr;
            g_ctx->PSSetShaderResources(0, 1, &n);
        }
    }

    // ================= COLOR PASS =================
    g_ctx->ClearRenderTargetView(g_rtv, clear);

    if (g_colorSRV || (g_depthPreview && g_depthSRV))
    {
        // Keep the DSV bound (depth test/write off) so the big fullscreen draw
        // is attributed to our depth buffer by ReShade's draw-call statistics.
        g_ctx->OMSetRenderTargets(1, &g_rtv, g_localDSV);
        g_ctx->OMSetDepthStencilState(g_dsOff, 0);
        g_ctx->VSSetShader(g_vs, nullptr, 0);

        if (g_depthPreview && g_depthSRV)
        {
            g_ctx->PSSetShader(g_psDepthView, nullptr, 0);
            SetRemap(g_depthTexW, g_depthTexH, p, true);
            g_ctx->PSSetShaderResources(0, 1, &g_depthSRV);
            g_ctx->PSSetSamplers(0, 1, &g_sampPoint);
        }
        else
        {
            g_ctx->PSSetShader(g_psColor, nullptr, 0);
            SetRemap(g_colorTexW, g_colorTexH, p, false);
            g_ctx->PSSetShaderResources(0, 1, &g_colorSRV);
            g_ctx->PSSetSamplers(0, 1, &g_sampLinear);
        }
        DrawTri();
        ID3D11ShaderResourceView* n = nullptr;
        g_ctx->PSSetShaderResources(0, 1, &n);
    }

    // ================= ImGui =================
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    if (g_showFPS)
    {
        ImGui::SetNextWindowBgAlpha(0.35f);
        ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Always);
        ImGui::Begin("##fps", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoNav);
        ImGui::TextColored(
            ImVec4(g_fpsColor[0], g_fpsColor[1], g_fpsColor[2], g_fpsColor[3]),
            "%.0f fps", g_fps);
        ImGui::End();
    }

    if (g_menu)
    {
        ImGui::SetNextWindowSize(ImVec2(400, 0), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowBgAlpha(0.95f);

        if (ImGui::Begin("Overlay Control", &g_menu,
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextColored(ImVec4(0.50f, 0.50f, 0.50f, 1.0f), "SYSTEM STATUS");
            ImGui::Separator();
            ImGui::Text("GPU Device: %s", g_gpuName.c_str());
            ImGui::Text("Status: %s", OverlayStatus(p));

            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.50f, 0.50f, 0.50f, 1.0f), "OPTIONS");
            ImGui::Separator();

            ImGui::Checkbox("Depth preview (F3)", &g_depthPreview);

            if (ImGui::Checkbox("Show FPS", &g_showFPS))
                SaveSettings();
            ImGui::SameLine(ImGui::GetWindowWidth() - 120);
            ImGui::PushItemWidth(80);
            ImGui::ColorEdit3("##fpsc", g_fpsColor,
                ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
            ImGui::PopItemWidth();

            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.50f, 0.50f, 0.50f, 1.0f), "KEYBINDS");
            ImGui::Separator();

            char minLabel[128];
            snprintf(minLabel, sizeof(minLabel),
                g_waitKeyTarget == 1 ? "Overlay On/Off Key: [press...]" : "Overlay On/Off Key: [%s]",
                VkKeyName(g_minimizeKey));
            if (ImGui::Button(minLabel, ImVec2(-1, 28)))
            {
                g_waitKeyTarget = 1;
                g_waitKeyStart = GetTickCount();
            }
            if (g_waitKeyTarget)
                ImGui::TextColored(ImVec4(0.83f, 0.83f, 0.83f, 1), "Press any key (ESC to cancel)");

            ImGui::Spacing();
            ImGui::Separator();
            if (ImGui::Button("Exit Overlay", ImVec2(-1, 30)))
                g_running = false;
        }
        ImGui::End();
    }

    static bool lastMenuState = false;
    if (lastMenuState != g_menu)
    {
        ApplyWindowMode();
        if (!g_menu && !g_reshadeInput) FocusRoblox();
        lastMenuState = g_menu;
    }

    ImGui::Render();
    if (g_menu || g_showFPS)
    {
        g_ctx->OMSetRenderTargets(1, &g_rtv, g_localDSV);
        g_ctx->OMSetDepthStencilState(g_dsOff, 0);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }

    // Leave the depth buffer BOUND at Present time. ReShade's generic-depth
    // add-on looks at the depth-stencil that is current when the frame ends.
    g_ctx->OMSetRenderTargets(1, &g_rtv, g_localDSV);

    g_swap->Present(g_vsync ? 1 : 0, 0);
}

// ===========================================================================
//  Window / target handling
// ===========================================================================
static HWND FindTarget()
{
    HWND h = FindWindowA(nullptr, "Roblox");
    if (h) GetWindowThreadProcessId(h, &g_targetPid);
    return h;
}

static RECT TargetRect()
{
    RECT r = { 100,100,100 + (LONG)g_width,100 + (LONG)g_height };
    if (!g_desktop && g_target && IsWindow(g_target))
    {
        GetWindowThreadProcessId(g_target, &g_targetPid);
        GetWindowRect(g_target, &r);
    }
    else if (g_desktop)
        SystemParametersInfoA(SPI_GETWORKAREA, 0, &r, 0);
    return r;
}

static void FocusRoblox()
{
    if (g_target && IsWindow(g_target))
        SetForegroundWindow(g_target);
}

static bool ForegroundIsRobloxOrOverlay()
{
    HWND fg = GetForegroundWindow();
    if (!fg)              return true;
    if (fg == g_hwnd)     return true;

    DWORD fgPid = 0;
    GetWindowThreadProcessId(fg, &fgPid);
    if (fgPid == GetCurrentProcessId()) return true;

    if (g_target && IsWindow(g_target))
    {
        DWORD robloxPid = 0;
        GetWindowThreadProcessId(g_target, &robloxPid);
        if (robloxPid) g_targetPid = robloxPid;
        if (fg == g_target || IsChild(g_target, fg)) return true;
    }
    if (g_targetPid && fgPid == g_targetPid) return true;
    return false;
}

static void UpdateForegroundVisibility()
{
    if (g_desktop || !g_hwnd) return;

    bool shouldShow = g_overlayEnabled && ForegroundIsRobloxOrOverlay();
    if (!shouldShow)
    {
        if (!g_overlayHiddenForForeground)
        {
            ShowWindow(g_hwnd, SW_HIDE);
            SetWindowPos(g_hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            g_overlayHiddenForForeground = true;
        }
        return;
    }
    if (g_overlayHiddenForForeground)
    {
        ShowWindow(g_hwnd, SW_SHOWNA);
        g_overlayHiddenForForeground = false;
        g_lastRect = { 0,0,0,0 };
    }
}

static LONG_PTR ExStyle()
{
    LONG_PTR e = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED;
    if (!g_menu && !g_reshadeInput)
        e |= WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
    return e;
}

static void ApplyWindowMode()
{
    if (!g_hwnd) return;
    SetWindowLongPtrA(g_hwnd, GWL_EXSTYLE, ExStyle());

    if (g_transparent && !g_menu && !g_reshadeInput)
        SetLayeredWindowAttributes(g_hwnd, RGB(0, 0, 0), 0, LWA_COLORKEY);
    else
        SetLayeredWindowAttributes(g_hwnd, 0, 255, LWA_ALPHA);

    SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED | SWP_SHOWWINDOW |
        ((g_menu || g_reshadeInput) ? 0 : SWP_NOACTIVATE));

    if (!g_menu && !g_reshadeInput) FocusRoblox();
}

static void SetMenu(bool v)
{
    g_menu = v;
    if (v) g_reshadeInput = false;
    ApplyWindowMode();
    if (v) { SetForegroundWindow(g_hwnd); SetFocus(g_hwnd); }
    else FocusRoblox();
}

static void SendKeyToRoblox(WORD vk, bool forceSend = false)
{
    if (!g_target || !IsWindow(g_target)) return;
    ShowWindow(g_target, SW_SHOW);
    SetForegroundWindow(g_target);
    Sleep(30);
    if (forceSend || GetForegroundWindow() != g_target)
    {
        INPUT in[2] = {};
        in[0].type = INPUT_KEYBOARD; in[0].ki.wVk = vk;
        in[1].type = INPUT_KEYBOARD; in[1].ki.wVk = vk; in[1].ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(2, in, sizeof(INPUT));
    }
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (g_menu && ImGui_ImplWin32_WndProcHandler(h, m, w, l))
        return TRUE;
    if (m == WM_DESTROY) { g_running = false; PostQuitMessage(0); return 0; }
    return DefWindowProcA(h, m, w, l);
}

static bool InitWindow()
{
    if (!g_desktop)
    {
        g_target = FindTarget();
        if (!g_target) { Log("[Window] Roblox not found"); return false; }
    }
    RECT r = TargetRect();
    g_width = std::max<UINT>(1, r.right - r.left);
    g_height = std::max<UINT>(1, r.bottom - r.top);

    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = "FixedHookStatusPipeOverlay";
    RegisterClassExA(&wc);

    g_hwnd = CreateWindowExA(ExStyle(), wc.lpszClassName, "FeatureModule Overlay",
        WS_POPUP, r.left, r.top, g_width, g_height,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_hwnd) return false;

    MARGINS ma = { -1,-1,-1,-1 };
    DwmExtendFrameIntoClientArea(g_hwnd, &ma);
    ApplyWindowMode();
    ShowWindow(g_hwnd, SW_SHOWNA);
    return true;
}

static bool InitD3D()
{
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = g_width;
    sd.BufferDesc.Height = g_height;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = g_hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL fl;
    D3D_FEATURE_LEVEL lv[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, lv, 2, D3D11_SDK_VERSION,
        &sd, &g_swap, &g_dev, &fl, &g_ctx);
    if (FAILED(hr))
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, lv, 2, D3D11_SDK_VERSION,
            &sd, &g_swap, &g_dev, &fl, &g_ctx);
    if (FAILED(hr)) return false;

    ID3D11Texture2D* bb = nullptr;
    g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb);
    g_dev->CreateRenderTargetView(bb, nullptr, &g_rtv);
    bb->Release();
    CreateLocalDepth(g_width, g_height);

    ID3DBlob* b = nullptr;
    if (Compile(VS_SRC, "vs_5_0", &b)) { g_dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_vs); b->Release(); }
    if (Compile(PS_COLOR, "ps_5_0", &b)) { g_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_psColor); b->Release(); }
    if (Compile(PS_DEPTH, "ps_5_0", &b)) { g_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_psDepth); b->Release(); }
    if (Compile(PS_DEPTH_VIEW, "ps_5_0", &b)) { g_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_psDepthView); b->Release(); }

    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = sizeof(RemapCB);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_dev->CreateBuffer(&cbd, nullptr, &g_cb);

    D3D11_DEPTH_STENCIL_DESC ds = {};
    ds.DepthEnable = TRUE;
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
    g_dev->CreateDepthStencilState(&ds, &g_dsWrite);
    ds = {}; ds.DepthEnable = FALSE;
    g_dev->CreateDepthStencilState(&ds, &g_dsOff);

    D3D11_SAMPLER_DESC sp = {};
    sp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sp.AddressU = sp.AddressV = sp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    g_dev->CreateSamplerState(&sp, &g_sampLinear);
    sp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    g_dev->CreateSamplerState(&sp, &g_sampPoint);

    QueryGpuNameFromDevice();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    {
        ImGuiStyle& st = ImGui::GetStyle();
        st.WindowRounding = 8.0f;
        st.FrameRounding = 6.0f;
        st.GrabRounding = 4.0f;
        st.WindowBorderSize = 1.0f;
        st.FrameBorderSize = 0.0f;
        st.WindowPadding = ImVec2(14, 14);
        st.ItemSpacing = ImVec2(10, 8);
        st.ScrollbarSize = 6.0f;
        st.ScrollbarRounding = 3.0f;
        ImVec4* c = st.Colors;
        c[ImGuiCol_WindowBg] = ImVec4(0.055f, 0.055f, 0.055f, 0.94f);
        c[ImGuiCol_ChildBg] = ImVec4(0.065f, 0.065f, 0.065f, 1.0f);
        c[ImGuiCol_PopupBg] = ImVec4(0.07f, 0.07f, 0.07f, 0.96f);
        c[ImGuiCol_Border] = ImVec4(0.14f, 0.14f, 0.14f, 0.60f);
        c[ImGuiCol_FrameBg] = ImVec4(0.08f, 0.08f, 0.08f, 1.0f);
        c[ImGuiCol_FrameBgHovered] = ImVec4(0.11f, 0.11f, 0.11f, 1.0f);
        c[ImGuiCol_FrameBgActive] = ImVec4(0.14f, 0.14f, 0.14f, 1.0f);
        c[ImGuiCol_TitleBg] = ImVec4(0.055f, 0.055f, 0.055f, 1.0f);
        c[ImGuiCol_TitleBgActive] = ImVec4(0.07f, 0.07f, 0.07f, 1.0f);
        c[ImGuiCol_TitleBgCollapsed] = ImVec4(0.055f, 0.055f, 0.055f, 0.75f);
        c[ImGuiCol_MenuBarBg] = ImVec4(0.08f, 0.08f, 0.08f, 1.0f);
        c[ImGuiCol_ScrollbarBg] = ImVec4(0.055f, 0.055f, 0.055f, 0.60f);
        c[ImGuiCol_ScrollbarGrab] = ImVec4(0.20f, 0.20f, 0.20f, 1.0f);
        c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.28f, 0.28f, 0.28f, 1.0f);
        c[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.35f, 0.35f, 0.35f, 1.0f);
        c[ImGuiCol_CheckMark] = ImVec4(0.83f, 0.83f, 0.83f, 1.0f);
        c[ImGuiCol_SliderGrab] = ImVec4(0.55f, 0.55f, 0.55f, 1.0f);
        c[ImGuiCol_SliderGrabActive] = ImVec4(0.75f, 0.75f, 0.75f, 1.0f);
        c[ImGuiCol_Button] = ImVec4(0.10f, 0.10f, 0.10f, 1.0f);
        c[ImGuiCol_ButtonHovered] = ImVec4(0.15f, 0.15f, 0.15f, 1.0f);
        c[ImGuiCol_ButtonActive] = ImVec4(0.20f, 0.20f, 0.20f, 1.0f);
        c[ImGuiCol_Header] = ImVec4(0.10f, 0.10f, 0.10f, 1.0f);
        c[ImGuiCol_HeaderHovered] = ImVec4(0.14f, 0.14f, 0.14f, 1.0f);
        c[ImGuiCol_HeaderActive] = ImVec4(0.18f, 0.18f, 0.18f, 1.0f);
        c[ImGuiCol_Separator] = ImVec4(0.14f, 0.14f, 0.14f, 0.50f);
        c[ImGuiCol_SeparatorHovered] = ImVec4(0.30f, 0.30f, 0.30f, 0.50f);
        c[ImGuiCol_SeparatorActive] = ImVec4(0.40f, 0.40f, 0.40f, 0.50f);
        c[ImGuiCol_ResizeGrip] = ImVec4(0.20f, 0.20f, 0.20f, 0.20f);
        c[ImGuiCol_ResizeGripHovered] = ImVec4(0.35f, 0.35f, 0.35f, 0.40f);
        c[ImGuiCol_ResizeGripActive] = ImVec4(0.50f, 0.50f, 0.50f, 0.40f);
        c[ImGuiCol_Text] = ImVec4(0.83f, 0.83f, 0.83f, 1.0f);
        c[ImGuiCol_TextDisabled] = ImVec4(0.40f, 0.40f, 0.40f, 1.0f);
    }
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);
    return true;
}

static void Follow()
{
    if (g_desktop) return;
    UpdateForegroundVisibility();
    if (!g_overlayEnabled || g_overlayHiddenForForeground) return;
    DWORD now = GetTickCount();
    if (now - g_lastFollow < 50) return;
    g_lastFollow = now;
    if (!g_target || !IsWindow(g_target)) { g_target = FindTarget(); return; }
    RECT r = TargetRect();
    if (memcmp(&r, &g_lastRect, sizeof(r)))
    {
        g_lastRect = r;
        SetWindowPos(g_hwnd, HWND_TOPMOST,
            r.left, r.top, r.right - r.left, r.bottom - r.top,
            (g_menu || g_reshadeInput) ? SWP_SHOWWINDOW : (SWP_SHOWWINDOW | SWP_NOACTIVATE));
    }
}

static void RequestOverlayResync(const char* reason, DWORD delayMs)
{
    Log("[Resync] requested: %s", reason ? reason : "manual");
    g_menu = false;
    g_reshadeInput = false;
    ApplyWindowMode();
    if (g_hwnd) ShowWindow(g_hwnd, SW_HIDE);
    g_lastRect = { 0,0,0,0 };
    g_resyncDueTick = GetTickCount() + delayMs;
}

static void ProcessOverlayResync()
{
    if (!g_resyncDueTick) return;
    DWORD now = GetTickCount();
    if ((LONG)(now - g_resyncDueTick) < 0) return;
    g_resyncDueTick = 0;

    if (!g_desktop) g_target = FindTarget();
    RECT r = TargetRect();
    int nw = std::max<int>(1, (int)(r.right - r.left));
    int nh = std::max<int>(1, (int)(r.bottom - r.top));
    SetWindowPos(g_hwnd, HWND_TOPMOST, r.left, r.top, nw, nh, SWP_SHOWWINDOW | SWP_NOACTIVATE);

    // Force the shared resources to be re-opened: after a fullscreen
    // transition the layer recreates its shared textures and legacy shared
    // handle values can be recycled, leaving us with an SRV that points at a
    // destroyed texture (black depth -> dead SSR).
    SafeRelease(g_colorSRV); SafeRelease(g_colorTex);
    SafeRelease(g_depthSRV); SafeRelease(g_depthTex);
    g_lastColorHandle = g_lastDepthHandle = 0;
    g_lastColorW = g_lastColorH = g_lastDepthW = g_lastDepthH = 0;
    g_colorTexW = g_colorTexH = g_depthTexW = g_depthTexH = 0;

    ApplyWindowMode();
    if (g_overlayEnabled && ForegroundIsRobloxOrOverlay())
    {
        ShowWindow(g_hwnd, SW_SHOWNA);
        g_overlayHiddenForForeground = false;
    }
    Log("[Resync] applied: %dx%d at %ld,%ld", nw, nh, r.left, r.top);
}

static void Hotkeys()
{
    if (CaptureKeyIfWaiting())
        return;

    static bool pMenu = false, pF8 = false, pF1 = false, pF11 = false;

    bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    bool tabDown = (GetAsyncKeyState(VK_TAB) & 0x8000) != 0;
    bool menuCombo = shift && tabDown;
    bool f8 = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    bool minKeyDown = (GetAsyncKeyState(g_minimizeKey) & 0x8000) != 0;
    bool f11 = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;

    if (menuCombo && !pMenu && !g_reshadeInput)
        SetMenu(!g_menu);

    if (f8 && !pF8)
    {
        g_reshadeInput = !g_reshadeInput;
        if (g_reshadeInput) g_menu = false;
        ApplyWindowMode();
    }

    if (minKeyDown && !pF1)
    {
        g_overlayEnabled = !g_overlayEnabled;
        g_menu = false;
        g_reshadeInput = false;
        if (!g_overlayEnabled)
        {
            ShowWindow(g_hwnd, SW_HIDE);
            SetWindowPos(g_hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            FocusRoblox();
            Log("[Overlay] disabled");
        }
        else
        {
            g_overlayHiddenForForeground = false;
            g_lastRect = { 0,0,0,0 };
            ShowWindow(g_hwnd, SW_SHOWNA);
            ApplyWindowMode();
            Log("[Overlay] enabled");
        }
    }

    if (f11 && !pF11)
    {
        g_menu = false;
        g_reshadeInput = false;
        ApplyWindowMode();
        SendKeyToRoblox(VK_F11, true);
        g_overlayEnabled = true;
        RequestOverlayResync("F11 fullscreen transition", 1600);
    }

    // F2 now flips the *documented* depth convention and syncs ReShade.ini,
    // so the shader and ReShade can never disagree any more.
    if (GetAsyncKeyState(VK_F2) & 1)
    {
        g_depthReversed = !g_depthReversed;
        SaveSettings();
        Log("[Depth] reversed = %d (reload effects in ReShade)", (int)g_depthReversed);
    }

    if (GetAsyncKeyState(VK_F3) & 1)
        g_depthPreview = !g_depthPreview;

    if (GetAsyncKeyState(VK_END) & 1)
        g_running = false;

    pMenu = menuCombo; pF8 = f8; pF1 = minKeyDown; pF11 = f11;
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    g_console = HasArg("/console");
    g_desktop = HasArg("/desktop");
    g_menu = HasArg("/menu");
    g_transparent = HasArg("/transparent");
    if (HasArg("/no-depth-heuristic")) g_depthHeuristicDraws = 0;
    if (HasArg("/no-vsync")) g_vsync = false;
    // /low-perf no longer kills the depth pass (that silently disabled SSR);
    // it only drops the heuristic padding draws.
    if (HasArg("/low-perf")) { g_lowPerf = true; g_depthHeuristicDraws = 0; }
    if (HasArg("/no-depth")) g_depthPassEnabled = false;

    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    char* slash = strrchr(exe, '\\');
    if (slash)
    {
        *(slash + 1) = '\0';
        snprintf(g_iniPath, MAX_PATH, "%soverlay_settings.ini", exe);
        snprintf(g_reshadeIniPath, MAX_PATH, "%sReShade.ini", exe);
    }

    LoadSettings();
    WriteReShadeConfig();

    if (g_console)
    {
        AllocConsole();
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    }
    Log("FeatureModule Overlay starting (menu=Shift+Tab, minimizeKey=%d=%s, reversedDepth=%d)",
        g_minimizeKey, VkKeyName(g_minimizeKey), (int)g_depthReversed);

    if (!InitWindow()) { Log("[Window] init failed"); return 1; }
    if (!InitD3D()) { Log("[D3D] init failed");    return 2; }

    g_pipeThread = std::thread(PipeThread);

    MSG msg = {};
    while (g_running)
    {
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
            if (msg.message == WM_QUIT) g_running = false;
        }
        Hotkeys();
        Follow();
        ProcessOverlayResync();
        if (g_overlayEnabled && !g_overlayHiddenForForeground)
        {
            PipePayload fp;
            { std::lock_guard<std::mutex> lk(g_pipeMutex); fp = g_payload; }
            UpdateFPS(fp.frameCount);
            Render(fp);
        }
        else
        {
            Sleep(10);
        }
    }

    g_pipeStop = true;
    if (g_pipeThread.joinable()) g_pipeThread.join();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    SafeRelease(g_colorSRV);  SafeRelease(g_colorTex);
    SafeRelease(g_depthSRV);  SafeRelease(g_depthTex);
    SafeRelease(g_rtv);
    SafeRelease(g_localDepthSRV);
    SafeRelease(g_localDSV);  SafeRelease(g_localDepth);
    SafeRelease(g_dsWrite);   SafeRelease(g_dsOff);
    SafeRelease(g_sampLinear); SafeRelease(g_sampPoint);
    SafeRelease(g_cb);
    SafeRelease(g_vs);
    SafeRelease(g_psColor);   SafeRelease(g_psDepth);  SafeRelease(g_psDepthView);
    SafeRelease(g_swap);      SafeRelease(g_ctx);       SafeRelease(g_dev);

    if (g_console) FreeConsole();
    return 0;
}

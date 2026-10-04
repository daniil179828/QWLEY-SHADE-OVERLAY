#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <tlhelp32.h>
#include <cwchar>
#include <dwmapi.h>
#include <mmsystem.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <dxgi1_4.h>
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
#include <chrono>
#include <condition_variable>
#include <cfloat>
#include <cmath>

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "winmm.lib")

#define PIPE_NAME       L"\\\\.\\pipe\\qwleyshade"
#define FLAG_COLOR_OK   (1u << 0)
#define FLAG_DEPTH_OK   (1u << 1)
#define FLAG_FPS_OK     (1u << 2)
#define FLAG_VIEWPORT_OK (1u << 4)
#define PAYLOAD_SIZE    0x40u

extern "C" {
    __declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
    __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

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

static HWND   g_hwnd = nullptr;
static HWND   g_target = nullptr;
static DWORD  g_targetPid = 0;
static std::vector<DWORD> g_targetPids;
static DWORD  g_targetPidTick = 0;
static std::string g_targetExeName = "";
static bool   g_console = false;
static bool   g_desktop = false;
static bool   g_running = true;
static bool   g_menu = false;
static bool   g_reshadeInput = false;
static bool   g_transparent = false;
static bool   g_overlayEnabled = true;
static bool   g_overlayHiddenForForeground = false;
static bool   g_vsync = true;
static UINT   g_maxOverlayFps = 60;
static bool   g_overlayFpsExplicit = false;
static bool   g_uiLayoutReset = true;
static bool   g_showKeyHelp = true;
static bool   g_highResolutionTimer = false;
enum class PerfTier { Auto, Low, Balanced, High };
static PerfTier g_requestedPerfTier = PerfTier::Auto;
static const char* g_activePerfProfile = "Auto";
static bool   g_perfTierFromCmdline = false;
static bool   g_gpuSoftware = false;
static UINT   g_displayRefreshHz = 60;
static SIZE_T g_gpuDedicatedMB = 0;
static double g_overlayFrameCostMs = 0.0;

static UINT  g_menuActiveFps = 60;
static UINT  g_menuIdleFps = 20;
static UINT  g_counterUiFps = 8;
static DWORD g_statsSampleIntervalMs = 150;
static DWORD g_vramQueryIntervalMs = 1500;

static UINT   g_width = 1280, g_height = 720;
static DWORD  g_lastFollow = 0;
static RECT   g_lastRect = { 0,0,0,0 };

static bool  g_depthReversed = true;
static bool  g_depthUpsideDown = false;
static bool  g_depthLogarithmic = false;
static float g_depthFarPlane = 1000.0f;
static bool  g_depthPreview = false;
static bool  g_useViewportRemap = true;
static bool  g_depthPassEnabled = true;
static bool  g_fastDepthFinal = false;

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
static ID3D11Buffer* g_cbColor = nullptr;
static ID3D11Buffer* g_cbCombined = nullptr;
static ID3D11VertexShader* g_vs = nullptr;
static ID3D11PixelShader* g_psDepth = nullptr;
static ID3D11PixelShader* g_psColorDepth = nullptr;
static ID3D11PixelShader* g_psColor = nullptr;
static ID3D11PixelShader* g_psDepthView = nullptr;

static HWND g_uiHwnd = nullptr;
static HWND g_fpsHwnd = nullptr;
static ID3D11Texture2D* g_uiRT = nullptr;
static ID3D11RenderTargetView* g_uiRTV = nullptr;
static ID3D11ShaderResourceView* g_uiRTSRV = nullptr;
static ID3D11Texture2D* g_uiRT2 = nullptr;
static ID3D11RenderTargetView* g_uiRTV2 = nullptr;

static constexpr int kUiReadbackSlots = 3;
struct UiReadbackSlot
{
    ID3D11Texture2D* texture = nullptr;
    bool pending = false;
    uint64_t serial = 0;
    int x = 0, y = 0;
};
static UiReadbackSlot g_uiReadbacks[kUiReadbackSlots];
static uint64_t g_uiReadbackSerial = 0;
static void ReleaseUiReadbacks()
{
    for (auto& slot : g_uiReadbacks)
    {
        SafeRelease(slot.texture);
        slot.pending = false;
    }
}
static ID3D11PixelShader* g_psPremult = nullptr;
static HBITMAP g_uiBitmap = nullptr;
static HDC g_uiDc = nullptr;
static HGDIOBJ g_uiOriginalBitmap = nullptr;
static void* g_uiBits = nullptr;
static UINT g_uiW = 0, g_uiH = 0;
static UINT g_uiRegionW = 0, g_uiRegionH = 0;
static UINT g_uiBitmapW = 0, g_uiBitmapH = 0;
static ID3D11Buffer* g_uiCB = nullptr;
static HBITMAP g_fpsBitmap = nullptr;
static HDC g_fpsDc = nullptr;
static HGDIOBJ g_fpsOriginalBitmap = nullptr;
static void* g_fpsBits = nullptr;
static HFONT g_fpsFont = nullptr;

static ID3D11Texture2D* g_depthTex = nullptr;
static ID3D11ShaderResourceView* g_depthSRV = nullptr;
static ID3D11Texture2D* g_colorTex = nullptr;
static ID3D11ShaderResourceView* g_colorSRV = nullptr;
static uint64_t g_lastDepthHandle = 0, g_lastColorHandle = 0;
static uint32_t g_lastDepthW = 0, g_lastDepthH = 0, g_lastDepthFmt = 0;
static uint32_t g_lastColorW = 0, g_lastColorH = 0, g_lastColorFmt = 0;
static HRESULT  g_lastDepthOpenHR = S_OK, g_lastColorOpenHR = S_OK;
static HRESULT  g_lastDepthSRVHR = S_OK, g_lastColorSRVHR = S_OK;
static UINT     g_depthTexW = 0, g_depthTexH = 0;
static UINT     g_colorTexW = 0, g_colorTexH = 0;

static int      g_depthHeuristicDraws = 12;
static bool     g_showFPS = true;
static float    g_fpsColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
static int      g_fpsOffsetX = 10;
static int      g_fpsOffsetY = 10;
static int  g_minimizeKey = VK_F1;
static int  g_menuKey = VK_TAB;
static bool g_menuNeedsShift = true;
static bool g_menuNeedsCtrl = false;
static bool g_menuNeedsAlt = false;

static const DWORD kAutoGraceMs = 1500;
static const int   kAutoMaxAttempts = 6;
static bool g_autoArmed = false;
static int  g_autoAttemptCount = 0;
static DWORD g_autoLastAttemptTick = 0;
static DWORD g_autoWatchStartTick = 0;

static int   g_depthRecreateCount = 0;

static uint64_t g_framesPresented = 0;
static DWORD    g_firstPresentTick = 0;

static bool     g_autoDepthRebuildEnabled = true;

static int  g_waitKeyTarget = 0;
static DWORD g_waitKeyStart = 0;
static float g_fps = 0.0f;
static bool  g_sourceFpsAvailable = false;
static std::atomic<uint64_t> g_sourcePacketSerial{ 0 };
static std::string g_gpuName = "Unknown GPU";
static IDXGIAdapter3* g_adapter3 = nullptr;
static UINT64 g_vramUsageBytes = 0;
static UINT64 g_vramBudgetBytes = 0;
static DWORD g_lastVramQueryTick = 0;
static bool g_vramStatsAvailable = false;
static constexpr int kPerfHistorySize = 180;
static float g_overlayFrameHistory[kPerfHistorySize] = {};
static int g_perfHistoryOffset = 0;
static int g_perfHistoryCount = 0;
static DWORD g_resyncDueTick = 0;
static bool  g_resyncing = false;

static std::thread       g_pipeThread;
static std::mutex        g_pipeMutex;
static std::condition_variable g_pipeCv;
static std::atomic<bool> g_pipeStop{ false };
static std::atomic<bool>  g_pipeConnected{ false };

static std::atomic<bool>  g_pipeEverConnected{ false };
static std::atomic<bool>  g_hasPayload{ false };
static std::atomic<DWORD> g_lastPayloadTick{ 0 };
static PipePayload        g_payload;

static char g_iniPath[MAX_PATH] = {};
static char g_reshadeIniPath[MAX_PATH] = {};
static char g_imguiIniPath[MAX_PATH] = {};

static void SetMenu(bool v);
static void ForceDepthRebuildTick(const char* reason);
static void RequestOverlayResync(const char* reason, DWORD delayMs);
static void ApplyWindowMode();
static void FocusTarget();
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

static std::string TrimStr(const std::string& s);

static const char* const kDefaultTargetExes[] = {
    "RobloxPlayerBeta.exe",
    "RobloxPlayerLauncher.exe",
    "Voidstrap.exe",
    "voistrap.exe",
    "voistrap.bin.exe"
};
static const size_t kDefaultTargetExeCount =
sizeof(kDefaultTargetExes) / sizeof(kDefaultTargetExes[0]);

static std::vector<std::string> g_targetExes;
static std::string g_targetExeDisplay = "";

static std::string LowerAscii(std::string s)
{
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

static std::vector<std::string> ArgValues(const char* name)
{
    std::vector<std::string> out;
    const char* cmd = GetCommandLineA();
    if (!cmd) return out;

    char key[64] = {};
    snprintf(key, sizeof(key), "/%s:", name);

    const char* p = cmd;
    while ((p = strstr(p, key)) != nullptr)
    {
        p += strlen(key);
        char buf[1024] = {};
        size_t n = 0;
        while (*p && *p != ' ' && *p != '\t' && *p != '"' && n + 1 < sizeof(buf))
            buf[n++] = *p++;
        buf[n] = 0;

        size_t start = 0;
        while (start <= strlen(buf))
        {
            const size_t len = strlen(buf);
            size_t sep = len;
            for (size_t i = start; i < len; ++i)
                if (buf[i] == ',' || buf[i] == ';') { sep = i; break; }
            std::string item = LowerAscii(TrimStr(std::string(buf + start, sep - start)));
            if (!item.empty()) out.push_back(item);
            if (sep >= len) break;
            start = sep + 1;
        }
    }
    return out;
}

static void AddTargetExe(const std::string& raw)
{
    std::string v = LowerAscii(TrimStr(raw));
    if (v.empty()) return;
    const size_t slash = v.find_last_of("/\\");
    if (slash != std::string::npos) v = v.substr(slash + 1);
    if (v.empty()) return;
    for (const auto& e : g_targetExes)
        if (e == v) return;
    g_targetExes.push_back(v);
}

static void InitTargetList()
{
    g_targetExes.clear();
    for (size_t i = 0; i < kDefaultTargetExeCount; ++i)
        AddTargetExe(kDefaultTargetExes[i]);
    for (const auto& v : ArgValues("target"))
        AddTargetExe(v);

    g_targetExeDisplay.clear();
    for (size_t i = 0; i < g_targetExes.size(); ++i)
    {
        if (i) g_targetExeDisplay += ", ";
        g_targetExeDisplay += g_targetExes[i];
    }
}

static bool QueryProcessExeName(DWORD pid, char* out, size_t cap)
{
    if (out && cap) out[0] = 0;
    if (!pid || !out || cap < 2) return false;

    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    wchar_t path[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    const bool ok = QueryFullProcessImageNameW(h, 0, path, &size) != FALSE;
    CloseHandle(h);
    if (!ok) return false;

    const wchar_t* base = wcsrchr(path, L'\\');
    base = base ? base + 1 : path;
    if (WideCharToMultiByte(CP_UTF8, 0, base, -1, out, (int)cap, nullptr, nullptr) <= 0)
    {
        out[0] = 0;
        return false;
    }
    return out[0] != 0;
}

static bool IsProcessAlive(DWORD pid)
{
    if (!pid) return false;
    HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE, pid);
    if (!process) return false;
    const bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return alive;
}

static bool IsTargetProcess(DWORD pid, char* exeOut = nullptr, size_t exeCap = 0)
{
    char buf[MAX_PATH] = {};
    if (!QueryProcessExeName(pid, buf, sizeof(buf))) return false;

    std::string name = LowerAscii(buf);
    for (const auto& e : g_targetExes)
        if (name == e)
        {
            if (exeOut && exeCap) strncpy_s(exeOut, exeCap, buf, _TRUNCATE);
            return true;
        }
    return false;
}

static void RefreshTargetPids()
{
    const DWORD now = GetTickCount();

    if (now - g_targetPidTick < 2000) return;
    g_targetPidTick = now;

    g_targetPids.clear();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe))
    {
        do
        {
            char exe[MAX_PATH] = {};
            if (WideCharToMultiByte(CP_UTF8, 0, pe.szExeFile, -1, exe,
                (int)sizeof(exe), nullptr, nullptr) <= 0)
                continue;
            const std::string name = LowerAscii(exe);
            for (const auto& target : g_targetExes)
            {
                if (name == target)
                {
                    g_targetPids.push_back(pe.th32ProcessID);
                    break;
                }
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
}

static UINT QueryDisplayRefreshHz()
{
    UINT hz = 60;
    HMONITOR monitor = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXA mi = {};
    mi.cbSize = sizeof(mi);
    if (monitor && GetMonitorInfoA(monitor, &mi))
    {
        DEVMODEA dm = {};
        dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsA(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) &&
            dm.dmDisplayFrequency >= 30 && dm.dmDisplayFrequency <= 1000)
            hz = dm.dmDisplayFrequency;
    }
    return hz;
}

static const char* PerfTierName(PerfTier t)
{
    switch (t)
    {
    case PerfTier::Low:      return "low";
    case PerfTier::Balanced: return "balanced";
    case PerfTier::High:     return "high";
    default:                 break;
    }
    return "auto";
}

static void ApplyPerfProfile()
{
    PerfTier tier = g_requestedPerfTier;
    if (tier == PerfTier::Auto)
    {
        if (g_gpuSoftware || g_gpuDedicatedMB < 2048)
            tier = PerfTier::Low;
        else if (g_gpuDedicatedMB < 5120)
            tier = PerfTier::Balanced;
        else
            tier = PerfTier::High;
    }
    g_displayRefreshHz = QueryDisplayRefreshHz();
    // Keep heuristic draws intact so ReShade depth buffer detector never loses the buffer!
    g_depthHeuristicDraws = 12;
    g_fastDepthFinal = false;

    switch (tier)
    {
    case PerfTier::Low:
        g_activePerfProfile = (g_requestedPerfTier == PerfTier::Auto) ? "Auto (Low / Stable)" : "Low / Stable";
        g_menuActiveFps = 45;
        g_menuIdleFps = 10;
        g_counterUiFps = 10;
        g_statsSampleIntervalMs = 350;
        g_vramQueryIntervalMs = 3000;
        if (!g_overlayFpsExplicit)
            g_maxOverlayFps = std::clamp<UINT>(g_displayRefreshHz, 30u, 60u);
        break;
    case PerfTier::Balanced:
        g_activePerfProfile = (g_requestedPerfTier == PerfTier::Auto) ? "Auto (Balanced)" : "Balanced";
        g_menuActiveFps = std::min<UINT>(60, std::max<UINT>(45, g_displayRefreshHz));
        g_menuIdleFps = 15;
        g_counterUiFps = 20;
        g_statsSampleIntervalMs = 200;
        g_vramQueryIntervalMs = 1500;
        if (!g_overlayFpsExplicit)
            g_maxOverlayFps = std::clamp<UINT>(g_displayRefreshHz, 30u, 120u);
        break;
    case PerfTier::High:
        g_activePerfProfile = (g_requestedPerfTier == PerfTier::Auto) ? "Auto (High / Smooth)" : "High / Smooth";
        g_menuActiveFps = std::min<UINT>(120, std::max<UINT>(60, g_displayRefreshHz));
        g_menuIdleFps = 30;
        g_counterUiFps = 60;
        g_statsSampleIntervalMs = 100;
        g_vramQueryIntervalMs = 750;
        if (!g_overlayFpsExplicit)
            g_maxOverlayFps = std::clamp<UINT>(g_displayRefreshHz, 30u, 240u);
        break;
    default:
        g_activePerfProfile = "Auto";
        break;
    }

    g_overlayFrameCostMs = 0.0;
    Log("[Performance] profile=%s (tier=%d, requested=%d), overlayMaxFps=%u, depthHeuristic=%d, fastDepth=%d, VRAM=%zu MB",
        g_activePerfProfile, (int)tier, (int)g_requestedPerfTier,
        g_maxOverlayFps, g_depthHeuristicDraws, (int)g_fastDepthFinal, g_gpuDedicatedMB);
}

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
            g_gpuDedicatedMB = desc.DedicatedVideoMemory / (1024u * 1024u);
            g_displayRefreshHz = QueryDisplayRefreshHz();
            g_gpuSoftware = (desc.VendorId == 0x1414 && desc.DeviceId == 0x008C);
            ApplyPerfProfile();
        }
        if (!g_adapter3)
            adapter->QueryInterface(__uuidof(IDXGIAdapter3), (void**)&g_adapter3);
        adapter->Release();
    }
    dxgiDevice->Release();
}

static void UpdateSourceFPS()
{
    using Clock = std::chrono::steady_clock;
    static bool s_started = false;
    static uint64_t s_windowStartPacket = 0;
    static auto s_windowStartTime = Clock::now();
    static float s_smoothedFPS = 0.0f;

    const DWORD packetTick = g_lastPayloadTick.load(std::memory_order_acquire);
    const DWORD nowTick = GetTickCount();
    const bool validSource = g_pipeConnected.load(std::memory_order_acquire) &&
        g_hasPayload.load(std::memory_order_acquire) && packetTick &&
        nowTick - packetTick <= 750;
    if (!validSource)
    {
        s_started = false;
        s_smoothedFPS = 0.0f;
        g_fps = 0.0f;
        g_sourceFpsAvailable = false;
        return;
    }

    const uint64_t packetSerial = g_sourcePacketSerial.load(std::memory_order_acquire);
    const auto now = Clock::now();
    if (!s_started)
    {
        s_started = true;
        s_windowStartPacket = packetSerial;
        s_windowStartTime = now;
        g_sourceFpsAvailable = false;
        return;
    }

    const float elapsed = std::chrono::duration<float>(now - s_windowStartTime).count();
    if (elapsed < 0.075f) return;

    const uint64_t frameDelta = packetSerial - s_windowStartPacket;
    s_windowStartPacket = packetSerial;
    s_windowStartTime = now;

    if (frameDelta < 10000ull && elapsed < 2.0f)
    {
        const float measured = (float)frameDelta / elapsed;
        if (s_smoothedFPS <= 0.0f) s_smoothedFPS = measured;
        else s_smoothedFPS = s_smoothedFPS * 0.65f + measured * 0.35f;
        g_fps = s_smoothedFPS;
        g_sourceFpsAvailable = true;
    }
    else
    {
        s_smoothedFPS = 0.0f;
        g_sourceFpsAvailable = false;
    }
}

static void RecordPerformanceStats(double renderCostMs)
{
    if (renderCostMs < 0.0 || renderCostMs >= 1000.0) return;

    const DWORD now = GetTickCount();
    static DWORD lastSampleTick = 0;
    static double accumulatedMs = 0.0;
    static int accumulatedFrames = 0;
    accumulatedMs += renderCostMs;
    ++accumulatedFrames;

    if (now - lastSampleTick < g_statsSampleIntervalMs) return;
    lastSampleTick = now;
    const double sampleMs = accumulatedMs / (double)std::max(1, accumulatedFrames);
    accumulatedMs = 0.0;
    accumulatedFrames = 0;

    if (g_overlayFrameCostMs <= 0.0) g_overlayFrameCostMs = sampleMs;
    else g_overlayFrameCostMs = g_overlayFrameCostMs * 0.82 + sampleMs * 0.18;

    g_overlayFrameHistory[g_perfHistoryOffset] = (float)sampleMs;
    g_perfHistoryOffset = (g_perfHistoryOffset + 1) % kPerfHistorySize;
    g_perfHistoryCount = std::min(g_perfHistoryCount + 1, kPerfHistorySize);
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

static int MenuKeyTokenToVk(const std::string& raw)
{
    const std::string key = LowerAscii(TrimStr(raw));
    if (key.size() == 1 && key[0] >= 'a' && key[0] <= 'z')
        return 'A' + (key[0] - 'a');
    if (key.size() == 1 && key[0] >= '0' && key[0] <= '9')
        return '0' + (key[0] - '0');
    if (key.size() >= 2 && key[0] == 'f')
    {
        const int number = atoi(key.c_str() + 1);
        if (number >= 1 && number <= 24) return VK_F1 + number - 1;
    }
    if (key == "tab") return VK_TAB;
    if (key == "insert" || key == "ins") return VK_INSERT;
    if (key == "delete" || key == "del") return VK_DELETE;
    if (key == "home") return VK_HOME;
    if (key == "end") return VK_END;
    if (key == "pageup" || key == "pgup") return VK_PRIOR;
    if (key == "pagedown" || key == "pgdn") return VK_NEXT;
    if (key == "space") return VK_SPACE;
    if (key == "enter" || key == "return") return VK_RETURN;
    if (key == "escape" || key == "esc") return VK_ESCAPE;
    if (key == "backspace") return VK_BACK;
    if (key == "up") return VK_UP;
    if (key == "down") return VK_DOWN;
    if (key == "left") return VK_LEFT;
    if (key == "right") return VK_RIGHT;
    return 0;
}

static bool ParseMenuBinding(const char* text)
{
    if (!text || !text[0]) return false;
    bool shift = false, ctrl = false, alt = false;
    int keyVk = 0;
    std::string binding = text;
    size_t start = 0;
    while (start <= binding.size())
    {
        const size_t plus = binding.find('+', start);
        const std::string token = LowerAscii(TrimStr(binding.substr(start,
            plus == std::string::npos ? std::string::npos : plus - start)));
        if (token == "shift") shift = true;
        else if (token == "ctrl" || token == "control") ctrl = true;
        else if (token == "alt") alt = true;
        else
        {
            const int parsed = MenuKeyTokenToVk(token);
            if (!parsed || keyVk) return false;
            keyVk = parsed;
        }
        if (plus == std::string::npos) break;
        start = plus + 1;
    }
    if (!keyVk) return false;
    g_menuKey = keyVk;
    g_menuNeedsShift = shift;
    g_menuNeedsCtrl = ctrl;
    g_menuNeedsAlt = alt;
    return true;
}

static std::string MenuBindingText()
{
    std::string out;
    if (g_menuNeedsCtrl) out += "CTRL+";
    if (g_menuNeedsShift) out += "SHIFT+";
    if (g_menuNeedsAlt) out += "ALT+";

    if (g_menuKey >= 'A' && g_menuKey <= 'Z')
        out.push_back((char)g_menuKey);
    else if (g_menuKey >= '0' && g_menuKey <= '9')
        out.push_back((char)g_menuKey);
    else if (g_menuKey >= VK_F1 && g_menuKey <= VK_F24)
        out += "F" + std::to_string(g_menuKey - VK_F1 + 1);
    else
    {
        switch (g_menuKey)
        {
        case VK_TAB: out += "TAB"; break;
        case VK_INSERT: out += "INSERT"; break;
        case VK_DELETE: out += "DELETE"; break;
        case VK_HOME: out += "HOME"; break;
        case VK_END: out += "END"; break;
        case VK_PRIOR: out += "PAGEUP"; break;
        case VK_NEXT: out += "PAGEDOWN"; break;
        case VK_SPACE: out += "SPACE"; break;
        case VK_RETURN: out += "ENTER"; break;
        case VK_ESCAPE: out += "ESCAPE"; break;
        case VK_BACK: out += "BACKSPACE"; break;
        case VK_UP: out += "UP"; break;
        case VK_DOWN: out += "DOWN"; break;
        case VK_LEFT: out += "LEFT"; break;
        case VK_RIGHT: out += "RIGHT"; break;
        default: out += "TAB"; break;
        }
    }
    return out;
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
static void WriteReShadeConfig()
{
    if (!g_reshadeIniPath[0]) return;

    std::vector<IniSection> ini = IniLoad(g_reshadeIniPath);

    IniSet(ini, "INPUT", "KeyOverlay", "36,0,0,0");

    IniSet(ini, "DEPTH", "DepthCopyBeforeClears", "0");
    IniSet(ini, "DEPTH", "DepthCopyAtClearIndex", "0");
    IniSet(ini, "DEPTH", "DisableINTZ", "0");
    IniSet(ini, "DEPTH", "UseAspectRatioHeuristics", "1");
    IniSet(ini, "DEPTH", "DrawStatsHeuristic", "2");
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
    MergeDefine(defs, "RESHADE_DEPTH_INPUT_IS_REVERSED", "1");
    MergeDefine(defs, "RESHADE_DEPTH_INPUT_IS_LOGARITHMIC", g_depthLogarithmic ? "1" : "0");

    std::string joined;
    for (size_t i = 0; i < defs.size(); ++i)
    {
        if (i) joined += ",";
        joined += defs[i];
    }
    IniSet(ini, "GENERAL", "PreprocessorDefinitions", joined);

    IniSet(ini, "GENERAL", "PerformanceMode", "0");

    if (IniSave(g_reshadeIniPath, ini))
        Log("[ReShade] config written (%s), reversed=%d upsideDown=%d log=%d",
            g_reshadeIniPath, (int)g_depthReversed, (int)g_depthUpsideDown, (int)g_depthLogarithmic);
    else
        Log("[ReShade] cannot write %s", g_reshadeIniPath);
}
static void LoadSettings()
{
    if (!g_iniPath[0]) return;
    std::vector<IniSection> ini = IniLoad(g_iniPath);
    if (ini.empty()) return;
    g_uiLayoutReset = atoi(IniGet(ini, "UI", "LayoutVersion", "0")) < 4;
    g_showKeyHelp = atoi(IniGet(ini, "UI", "HasOpenedMenu", "0")) == 0;

    int v = atoi(IniGet(ini, "Keybinds", "Minimize", "0"));
    if (v > 0 && v < 256) g_minimizeKey = v;
    ParseMenuBinding(IniGet(ini, "Keybinds", "Menu", "SHIFT+TAB"));

    g_showFPS = atoi(IniGet(ini, "Style", "ShowFPS", "1")) != 0;
    const char* fpsX = IniGet(ini, "Style", "FpsPosX", "");
    const char* fpsY = IniGet(ini, "Style", "FpsPosY", "");
    if (fpsX[0]) g_fpsOffsetX = atoi(fpsX);
    if (fpsY[0]) g_fpsOffsetY = atoi(fpsY);
    if (!g_uiLayoutReset)
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
    else
    {
        g_fpsColor[0] = 1.0f;
        g_fpsColor[1] = 1.0f;
        g_fpsColor[2] = 1.0f;
        g_fpsColor[3] = 1.0f;
    }

    g_depthReversed = true;
    g_depthUpsideDown = atoi(IniGet(ini, "Depth", "UpsideDown", "0")) != 0;
    g_depthLogarithmic = atoi(IniGet(ini, "Depth", "Logarithmic", "0")) != 0;
    g_useViewportRemap = atoi(IniGet(ini, "Depth", "ViewportRemap", "1")) != 0;
    {
        float fp = (float)atof(IniGet(ini, "Depth", "FarPlane", "1000.0"));
        if (fp > 1.0f) g_depthFarPlane = fp;
    }

    if (!g_perfTierFromCmdline)
    {
        const char* prof = IniGet(ini, "Performance", "Profile", "");
        if (_stricmp(prof, "low") == 0)                             g_requestedPerfTier = PerfTier::Low;
        else if (_stricmp(prof, "balanced") == 0 ||
            _stricmp(prof, "medium") == 0)                     g_requestedPerfTier = PerfTier::Balanced;
        else if (_stricmp(prof, "high") == 0)                       g_requestedPerfTier = PerfTier::High;
        else if (_stricmp(prof, "auto") == 0)                       g_requestedPerfTier = PerfTier::Auto;
    }
}

static void SaveSettings()
{
    if (!g_iniPath[0]) return;
    std::vector<IniSection> ini;
    IniSet(ini, "Keybinds", "Minimize", std::to_string(g_minimizeKey));
    IniSet(ini, "Keybinds", "Menu", MenuBindingText());
    IniSet(ini, "UI", "LayoutVersion", g_uiLayoutReset ? "0" : "4");
    IniSet(ini, "UI", "HasOpenedMenu", g_showKeyHelp ? "0" : "1");
    IniSet(ini, "Performance", "Profile", PerfTierName(g_requestedPerfTier));
    IniSet(ini, "Performance", "OverlayLimit", "0");
    IniSet(ini, "Style", "ShowFPS", g_showFPS ? "1" : "0");
    IniSet(ini, "Style", "FpsPosX", std::to_string(g_fpsOffsetX));
    IniSet(ini, "Style", "FpsPosY", std::to_string(g_fpsOffsetY));
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
    Log("[Pipe] waiting for %ls", PIPE_NAME);
    while (!g_pipeStop.load())
    {
        HANDLE h = INVALID_HANDLE_VALUE;
        while (h == INVALID_HANDLE_VALUE && !g_pipeStop.load())
        {
            h = CreateFileW(PIPE_NAME, GENERIC_READ, 0, nullptr,
                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE)
            {
                if (GetLastError() == ERROR_PIPE_BUSY) WaitNamedPipeW(PIPE_NAME, 500);
                else Sleep(300);
            }
        }
        if (g_pipeStop.load()) { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); break; }
        g_pipeConnected.store(true, std::memory_order_release);
        g_pipeEverConnected.store(true, std::memory_order_release);
        g_pipeCv.notify_all();
        Log("[Pipe] connected");
        while (!g_pipeStop.load())
        {
            uint8_t raw[PAYLOAD_SIZE] = {};
            if (!ReadExact(h, raw, sizeof(raw))) break;
            PipePayload p = NormalizePayload(raw);
            {
                std::lock_guard<std::mutex> lk(g_pipeMutex);
                g_payload = p;

                g_sourcePacketSerial.fetch_add(1, std::memory_order_release);
            }
            g_lastPayloadTick.store(GetTickCount(), std::memory_order_release);
            g_hasPayload.store(true, std::memory_order_release);
            g_pipeCv.notify_all();
        }
        CloseHandle(h);
        {
            std::lock_guard<std::mutex> lk(g_pipeMutex);
            g_payload = PipePayload{};
            g_sourcePacketSerial.fetch_add(1, std::memory_order_release);
        }
        g_lastPayloadTick.store(0, std::memory_order_release);
        g_hasPayload.store(false, std::memory_order_release);
        g_pipeConnected.store(false);
        g_pipeCv.notify_one();
        Log("[Pipe] disconnected");
        Sleep(300);
    }
}

struct RemapCB
{
    float uvScale[2];
    float uvBias[2];
    float misc[4];
};
static_assert(sizeof(RemapCB) == 32, "cb size");

struct CombinedRemapCB
{
    float colorUV[4];
    float depthUV[4];
    float misc[4];
};
static_assert(sizeof(CombinedRemapCB) == 48, "combined cb size");

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

static const char* PS_COLOR_DEPTH = R"HLSL(
cbuffer P : register(b0) {
    float4 gColorUV;
    float4 gDepthUV;
    float4 gMisc;
};
Texture2D<float4> ColorT : register(t0);
Texture2D<float> DepthT : register(t1);
SamplerState LinearS : register(s0);
SamplerState PointS : register(s1);
struct Out {
    float4 color : SV_Target;
    float depth : SV_Depth;
};
Out main(float4 p:SV_POSITION, float2 uv:TEXCOORD0) {
    Out o;
    const float2 colorUV = uv * gColorUV.xy + gColorUV.zw;
    const float2 depthUV = uv * gDepthUV.xy + gDepthUV.zw;
    o.color = float4(ColorT.SampleLevel(LinearS, colorUV, 0).rgb, 1.0);
    o.depth = saturate(DepthT.SampleLevel(PointS, depthUV, 0).r);
    return o;
}
)HLSL";

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

    float f = max(gMisc.y, 1.0);
    float lin = 1.0 / (d * (1.0 - 1.0 / f) + (1.0 / f));
    lin = saturate(lin / f);
    return float4(lin, lin, lin, 1);
}
)HLSL";

static const char* PS_PREMULT = R"HLSL(
cbuffer P : register(b0) { float4 gUV; float4 gMisc; };
Texture2D<float4> T : register(t0);
SamplerState S : register(s0);
float4 main(float4 p:SV_POSITION, float2 uv:TEXCOORD0):SV_Target {
    float2 c = uv * gUV.xy + gUV.zw;
    float4 s = T.SampleLevel(S, c, 0);
    return float4(s.rgb * s.a, s.a);
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

static ID3D11RenderTargetView* CreateBackBufferRTV()
{
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) || !bb)
        return nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    g_dev->CreateRenderTargetView(bb, nullptr, &rtv);
    bb->Release();
    return rtv;
}

static bool Resize(UINT w, UINT h)
{
    if (!w || !h) return false;
    if (w == g_width && h == g_height && g_rtv) return true;

    g_ctx->OMSetRenderTargets(0, nullptr, nullptr);
    SafeRelease(g_localDepthSRV); SafeRelease(g_localDSV); SafeRelease(g_localDepth);

    if (w != g_width || h != g_height)
    {
        SafeRelease(g_rtv);
        HRESULT hr = g_swap->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(hr))
        {
            Log("[D3D] ResizeBuffers(%ux%u) failed 0x%08lX; retrying next frame",
                w, h, (unsigned long)hr);
            if (!g_rtv) g_rtv = CreateBackBufferRTV();
            CreateLocalDepth(g_width, g_height);
            return false;
        }
        g_width = w; g_height = h;
    }

    if (!g_rtv) g_rtv = CreateBackBufferRTV();
    if (g_depthPassEnabled)
        CreateLocalDepth(g_width, g_height);
    Log("[D3D] resized to %ux%u", w, h);
    return true;
}

static void EnsureBackBufferMatchesWindow()
{
    if (!g_hwnd || !g_swap || !g_ctx) return;

    RECT c = {};
    if (!GetClientRect(g_hwnd, &c)) return;
    const UINT cw = (UINT)std::max<LONG>(1, c.right - c.left);
    const UINT ch = (UINT)std::max<LONG>(1, c.bottom - c.top);

    if (cw == g_width && ch == g_height && g_rtv) return;

    Log("[Geometry] client %ux%u vs back buffer %ux%u -> correcting",
        cw, ch, g_width, g_height);
    Resize(cw, ch);
}

static bool OpenSharedTex(uint64_t handleValue, ID3D11Texture2D** out, HRESULT* outHr)
{
    *out = nullptr;
    HANDLE h = (HANDLE)(uintptr_t)handleValue;
    HRESULT hr = g_dev->OpenSharedResource(h, __uuidof(ID3D11Texture2D), (void**)out);
    if (outHr) *outHr = hr;
    return SUCCEEDED(hr) && *out;
}

static void ReleaseDepthInput()
{
    if (!g_depthSRV && !g_depthTex && !g_lastDepthHandle) return;
    ID3D11ShaderResourceView* nullSRV = nullptr;
    g_ctx->PSSetShaderResources(0, 1, &nullSRV);
    SafeRelease(g_depthSRV);
    SafeRelease(g_depthTex);
    g_depthTexW = g_depthTexH = 0;
    g_lastDepthHandle = 0;
    g_lastDepthW = g_lastDepthH = g_lastDepthFmt = 0;
}

static void ReleaseColorInput()
{
    if (!g_colorSRV && !g_colorTex && !g_lastColorHandle) return;
    ID3D11ShaderResourceView* nullSRV = nullptr;
    g_ctx->PSSetShaderResources(0, 1, &nullSRV);
    SafeRelease(g_colorSRV);
    SafeRelease(g_colorTex);
    g_colorTexW = g_colorTexH = 0;
    g_lastColorHandle = 0;
    g_lastColorW = g_lastColorH = g_lastColorFmt = 0;
}

static bool IsDepthFresh(const PipePayload& p)
{
    static uint32_t lastFrame = 0;
    static DWORD lastAdvanceTick = 0;
    const DWORD now = GetTickCount();
    const DWORD packetTick = g_lastPayloadTick.load(std::memory_order_acquire);

    if (!g_pipeConnected.load(std::memory_order_acquire) ||
        !g_hasPayload.load(std::memory_order_acquire) ||
        !packetTick || now - packetTick > 750 ||
        !(p.flags & FLAG_DEPTH_OK) || !p.depthHandle)
    {
        lastAdvanceTick = 0;
        lastFrame = p.frameCount;
        return false;
    }
    if (p.flags & FLAG_FPS_OK)
    {
        if (!lastAdvanceTick || p.frameCount != lastFrame)
        {
            lastFrame = p.frameCount;
            lastAdvanceTick = now;
        }
        else if (now - lastAdvanceTick > 750)
        {
            return false;
        }
    }
    return true;
}

static void UpdateResources(const PipePayload& p)
{
    const bool colorValid = p.colorHandle && p.colorWidth && p.colorHeight;
    if (colorValid)
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
    else
    {
        ReleaseColorInput();
    }

    const bool depthValid = IsDepthFresh(p) &&
        (p.depthWidth || p.colorWidth) && (p.depthHeight || p.colorHeight);
    if (depthValid)
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
                Log("tex %ux%u payloadFmt=%u texFmt=%u srvFmt=%u srvHR=0x%08lX",
                    td.Width, td.Height, p.depthFormat, td.Format, s.Format,
                    (unsigned long)g_lastDepthSRVHR);
            }
            else Log(" Open failed 0x%08lX", (unsigned long)g_lastDepthOpenHR);
            g_lastDepthHandle = p.depthHandle;
            g_lastDepthW = dw; g_lastDepthH = dh;
            g_lastDepthFmt = p.depthFormat;
        }
    }
    else
    {
        ReleaseDepthInput();
    }
}

static void SetRemap(UINT texW, UINT texH, const PipePayload& p, bool invert,
    ID3D11Buffer* buffer)
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

    if (invert && g_depthUpsideDown)
    {
        cb.uvBias[1] += cb.uvScale[1];
        cb.uvScale[1] = -cb.uvScale[1];
    }

    cb.misc[0] = 0.0f;
    cb.misc[1] = g_depthFarPlane;
    cb.misc[2] = 0.0f;
    cb.misc[3] = 0.0f;

    static RemapCB lastDepth = {}, lastColor = {};
    static bool depthValid = false, colorValid = false;
    RemapCB& last = (buffer == g_cbColor) ? lastColor : lastDepth;
    bool& valid = (buffer == g_cbColor) ? colorValid : depthValid;
    if (!valid || memcmp(&last, &cb, sizeof(cb)) != 0)
    {
        D3D11_MAPPED_SUBRESOURCE m = {};
        if (SUCCEEDED(g_ctx->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        {
            memcpy(m.pData, &cb, sizeof(cb));
            g_ctx->Unmap(buffer, 0);
            last = cb;
            valid = true;
        }
    }
    g_ctx->PSSetConstantBuffers(0, 1, &buffer);
}

static void SetCombinedRemap(const PipePayload& p)
{
    CombinedRemapCB cb = {};
    cb.colorUV[0] = cb.colorUV[1] = 1.0f;
    cb.depthUV[0] = cb.depthUV[1] = 1.0f;

    if (g_useViewportRemap && (p.flags & FLAG_VIEWPORT_OK) &&
        p.vpWidth && p.vpHeight)
    {
        if (g_colorTexW && g_colorTexH)
        {
            cb.colorUV[0] = (float)p.vpWidth / (float)g_colorTexW;
            cb.colorUV[1] = (float)p.vpHeight / (float)g_colorTexH;
            cb.colorUV[2] = (float)p.vpX / (float)g_colorTexW;
            cb.colorUV[3] = (float)p.vpY / (float)g_colorTexH;
        }
        if (g_depthTexW && g_depthTexH)
        {
            cb.depthUV[0] = (float)p.vpWidth / (float)g_depthTexW;
            cb.depthUV[1] = (float)p.vpHeight / (float)g_depthTexH;
            cb.depthUV[2] = (float)p.vpX / (float)g_depthTexW;
            cb.depthUV[3] = (float)p.vpY / (float)g_depthTexH;
        }
    }
    if (g_depthUpsideDown)
    {
        cb.depthUV[3] += cb.depthUV[1];
        cb.depthUV[1] = -cb.depthUV[1];
    }

    cb.misc[0] = g_depthReversed ? 0.0f : 1.0f;
    cb.misc[1] = g_depthFarPlane;
    cb.misc[2] = 0.0f;
    cb.misc[3] = 0.0f;

    static CombinedRemapCB last = {};
    static bool valid = false;
    if (!valid || memcmp(&last, &cb, sizeof(cb)) != 0)
    {
        D3D11_MAPPED_SUBRESOURCE m = {};
        if (SUCCEEDED(g_ctx->Map(g_cbCombined, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        {
            memcpy(m.pData, &cb, sizeof(cb));
            g_ctx->Unmap(g_cbCombined, 0);
            last = cb;
            valid = true;
        }
    }
    g_ctx->PSSetConstantBuffers(0, 1, &g_cbCombined);
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

static void UpdateGpuMemoryStats()
{
    const DWORD now = GetTickCount();
    if (!g_adapter3 || now - g_lastVramQueryTick < g_vramQueryIntervalMs) return;
    g_lastVramQueryTick = now;
    DXGI_QUERY_VIDEO_MEMORY_INFO info = {};
    if (SUCCEEDED(g_adapter3->QueryVideoMemoryInfo(0,
        DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
    {
        g_vramUsageBytes = info.CurrentUsage;
        g_vramBudgetBytes = info.Budget;
        g_vramStatsAvailable = true;
    }
    else g_vramStatsAvailable = false;
}

static void HistoryStats(const float* values, float& minValue,
    float& average, float& maxValue)
{
    minValue = 0.0f; average = 0.0f; maxValue = 0.0f;
    if (g_perfHistoryCount <= 0) return;
    minValue = FLT_MAX;
    int validCount = 0;
    for (int i = 0; i < g_perfHistoryCount; ++i)
    {
        const float value = values[i];
        if (value <= 0.0f) continue;
        minValue = std::min(minValue, value);
        maxValue = std::max(maxValue, value);
        average += value;
        ++validCount;
    }
    if (minValue == FLT_MAX || validCount == 0) { minValue = 0.0f; return; }
    average /= (float)validCount;
}

static void UiSection(const char* title, const char* description = nullptr)
{
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.76f, 0.79f, 0.84f, 1.0f), "%s", title);
    if (description)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        ImGui::TextWrapped("%s", description);
        ImGui::PopStyleColor();
    }
    ImGui::Separator();
}

static void DrawGpuStatsTab()
{
    UiSection("Performance profile");
    const char* modes[] = { "Auto", "Low / Stable", "Balanced", "High / Smooth" };
    int mode = (int)g_requestedPerfTier;
    ImGui::PushItemWidth(-1);
    if (ImGui::Combo("##profile", &mode, modes, 4))
    {
        g_requestedPerfTier = (PerfTier)mode;
        ApplyPerfProfile();
        SaveSettings();
    }
    ImGui::PopItemWidth();
    ImGui::Text("Active: %s", g_activePerfProfile);

    UiSection("Adapter & memory");
    ImGui::TextWrapped("%s", g_gpuName.c_str());
    ImGui::Text("Dedicated VRAM: %zu MB", g_gpuDedicatedMB);
    UpdateGpuMemoryStats();
    if (g_vramStatsAvailable && g_vramBudgetBytes)
    {
        const double mb = 1024.0 * 1024.0;
        const float ratio = (float)std::min(1.0, (double)g_vramUsageBytes / g_vramBudgetBytes);
        char memory[96];
        snprintf(memory, sizeof(memory), "%.0f / %.0f MB", g_vramUsageBytes / mb, g_vramBudgetBytes / mb);
        ImGui::ProgressBar(ratio, ImVec2(-1, 24), memory);
    }
    else ImGui::TextDisabled("VRAM data unavailable");

    UiSection("Frame pacing");
    if (g_sourceFpsAvailable) ImGui::Text("%.0f FPS", g_fps);
    else ImGui::TextDisabled("-- FPS");
    ImGui::Text("Overlay frame: %.2f ms", g_overlayFrameCostMs);

    float minimum, average, maximum;
    HistoryStats(g_overlayFrameHistory, minimum, average, maximum);
    const int count = std::max(1, g_perfHistoryCount);
    const int offset = g_perfHistoryCount == kPerfHistorySize ? g_perfHistoryOffset : 0;
    ImGui::PlotLines("##cpu_history", g_overlayFrameHistory, count, offset, nullptr,
        0.0f, std::max(8.0f, maximum * 1.15f), ImVec2(-1, 76));
    ImGui::Text("Min %.2f  /  Avg %.2f  /  Max %.2f ms", minimum, average, maximum);

    const float budget = 1000.0f / (float)std::max<UINT>(30, g_maxOverlayFps);
    char timing[64];
    snprintf(timing, sizeof(timing), "%.2f / %.2f ms", g_overlayFrameCostMs, budget);
    ImGui::ProgressBar(std::clamp((float)(g_overlayFrameCostMs / budget), 0.0f, 1.0f),
        ImVec2(-1, 24), timing);
}

static const char* OverlayStatus(const PipePayload& p)
{
    static char text[192];
    if (!g_pipeConnected.load())   return "Searching pipe";
    if (!g_hasPayload.load())      return "Pipe connected, waiting payload";
    bool colorReady = g_colorSRV && p.colorHandle;
    bool depthReady = g_depthSRV && p.depthHandle && (p.flags & FLAG_DEPTH_OK);
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
    if (g_depthPassEnabled && !g_localDSV)
        CreateLocalDepth(g_width, g_height);

    float clear[4] = { 0,0,0, g_transparent ? 0.0f : 1.0f };
    g_ctx->ClearRenderTargetView(g_rtv, clear);
    ViewportFull();

    const float farValue = g_depthReversed ? 0.0f : 1.0f;

    const bool fusedColorDepth = g_depthPassEnabled && g_localDSV &&
        g_depthSRV && g_colorSRV && g_psColorDepth && !g_depthPreview;

    if (g_depthPassEnabled && g_localDSV)
    {
        g_ctx->ClearDepthStencilView(g_localDSV, D3D11_CLEAR_DEPTH, farValue, 0);

        if (g_depthSRV)
        {
            g_ctx->OMSetRenderTargets(1, &g_rtv, g_localDSV);
            g_ctx->OMSetDepthStencilState(g_dsWrite, 0);
            g_ctx->VSSetShader(g_vs, nullptr, 0);

            if (fusedColorDepth)
            {
                g_ctx->PSSetShader(g_psColorDepth, nullptr, 0);
                SetCombinedRemap(p);
                ID3D11ShaderResourceView* srvs[2] = { g_colorSRV, g_depthSRV };
                ID3D11SamplerState* samplers[2] = { g_sampLinear, g_sampPoint };
                g_ctx->PSSetShaderResources(0, 2, srvs);
                g_ctx->PSSetSamplers(0, 2, samplers);
                DrawTri();
                ID3D11ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };
                g_ctx->PSSetShaderResources(0, 2, nullSRVs);
            }
            else
            {
                g_ctx->PSSetShader(g_psDepth, nullptr, 0);
                SetRemap(g_depthTexW, g_depthTexH, p, true, g_cb);
                g_ctx->PSSetShaderResources(0, 1, &g_depthSRV);
                g_ctx->PSSetSamplers(0, 1, &g_sampPoint);
                DrawTri();
                ID3D11ShaderResourceView* nullSRV = nullptr;
                g_ctx->PSSetShaderResources(0, 1, &nullSRV);
            }

            if (g_depthHeuristicDraws > 0)
            {
                g_ctx->OMSetDepthStencilState(g_dsWrite, 0);
                g_ctx->PSSetShader(g_psDepth, nullptr, 0);
                SetRemap(g_depthTexW, g_depthTexH, p, true, g_cb);
                g_ctx->PSSetShaderResources(0, 1, &g_depthSRV);
                g_ctx->PSSetSamplers(0, 1, &g_sampPoint);

                D3D11_VIEWPORT tiny = {};
                tiny.Width = 1; tiny.Height = 1; tiny.MaxDepth = 1;
                g_ctx->RSSetViewports(1, &tiny);
                for (int i = 0; i < g_depthHeuristicDraws; i++) DrawTri();
                ViewportFull();
                ID3D11ShaderResourceView* nullDepthSRV = nullptr;
                g_ctx->PSSetShaderResources(0, 1, &nullDepthSRV);
            }
        }
    }

    if (!fusedColorDepth && (g_colorSRV || (g_depthPreview && g_depthSRV)))
    {
        g_ctx->OMSetRenderTargets(1, &g_rtv, g_localDSV);
        g_ctx->OMSetDepthStencilState(g_dsOff, 0);
        g_ctx->VSSetShader(g_vs, nullptr, 0);

        if (g_depthPreview && g_depthSRV)
        {
            g_ctx->PSSetShader(g_psDepthView, nullptr, 0);
            SetRemap(g_depthTexW, g_depthTexH, p, true, g_cb);
            g_ctx->PSSetShaderResources(0, 1, &g_depthSRV);
            g_ctx->PSSetSamplers(0, 1, &g_sampLinear);
        }
        else
        {
            g_ctx->PSSetShader(g_psColor, nullptr, 0);
            SetRemap(g_colorTexW, g_colorTexH, p, false, g_cbColor);
            g_ctx->PSSetShaderResources(0, 1, &g_colorSRV);
            g_ctx->PSSetSamplers(0, 1, &g_sampLinear);
        }
        DrawTri();
        ID3D11ShaderResourceView* nullSRV = nullptr;
        g_ctx->PSSetShaderResources(0, 1, &nullSRV);
    }

    g_ctx->OMSetRenderTargets(1, &g_rtv, g_localDSV);
}

static void RenderDepthFinalPass(const PipePayload& p)
{
    if (!g_depthPassEnabled || !g_localDSV || !g_depthSRV || !g_rtv) return;

    g_ctx->OMSetRenderTargets(1, &g_rtv, g_localDSV);
    g_ctx->OMSetDepthStencilState(g_dsWrite, 0);
    g_ctx->VSSetShader(g_vs, nullptr, 0);
    g_ctx->PSSetShader(g_psDepth, nullptr, 0);
    SetRemap(g_depthTexW, g_depthTexH, p, true, g_cb);
    g_ctx->PSSetShaderResources(0, 1, &g_depthSRV);
    g_ctx->PSSetSamplers(0, 1, &g_sampPoint);
    ViewportFull();

    DrawTri();
    DrawTri();
    ViewportFull();
    ID3D11ShaderResourceView* nullSRV = nullptr;
    g_ctx->PSSetShaderResources(0, 1, &nullSRV);
}

static RECT WindowClientRectOnScreen(HWND w)
{
    RECT out = { 0,0,0,0 };
    RECT client = {};
    if (!w || !GetClientRect(w, &client)) return out;
    POINT tl = { client.left, client.top };
    POINT br = { client.right, client.bottom };
    if (!ClientToScreen(w, &tl) || !ClientToScreen(w, &br)) return out;
    out.left = tl.x; out.top = tl.y;
    out.right = br.x; out.bottom = br.y;
    return out;
}

struct TargetScan
{
    HWND  best = nullptr;
    LONG  bestArea = 0;
    DWORD bestPid = 0;
};

static BOOL CALLBACK EnumTargetWindow(HWND w, LPARAM lp)
{
    TargetScan* scan = (TargetScan*)lp;

    if (!IsWindowVisible(w)) return TRUE;
    if (GetWindowTextLengthA(w) <= 0) return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (!pid || !IsTargetProcess(pid)) return TRUE;

    RECT c = WindowClientRectOnScreen(w);
    const LONG cw = c.right - c.left;
    const LONG ch = c.bottom - c.top;
    if (cw < 128 || ch < 128) return TRUE;
    const LONG area = cw * ch;

    if (area > scan->bestArea || !scan->best)
    {
        scan->best = w;
        scan->bestPid = pid;
        scan->bestArea = area;
    }
    return TRUE;
}

static HWND FindTarget()
{
    TargetScan scan;
    EnumWindows(EnumTargetWindow, (LPARAM)&scan);
    if (scan.best)
    {
        g_targetPid = scan.bestPid;
        return scan.best;
    }
    static const char* const kLegacyTitles[] = { "Roblox", "Voidstrap", "Voistrap" };
    for (const char* title : kLegacyTitles)
    {
        HWND h = FindWindowA(nullptr, title);
        if (h) { GetWindowThreadProcessId(h, &g_targetPid); return h; }
    }
    return nullptr;
}

static RECT TargetRect()
{
    RECT r = { 100,100,100 + (LONG)g_width,100 + (LONG)g_height };
    if (!g_desktop && g_target && IsWindow(g_target))
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(g_target, &pid);
        if (pid) g_targetPid = pid;
        RECT s = WindowClientRectOnScreen(g_target);
        if (s.right > s.left && s.bottom > s.top)
        {
            r.left = s.left; r.top = s.top;
            r.right = s.right; r.bottom = s.bottom;
        }
    }
    else if (g_desktop)
        SystemParametersInfoA(SPI_GETWORKAREA, 0, &r, 0);
    return r;
}

static void FocusTarget()
{
    if (g_target && IsWindow(g_target))
        SetForegroundWindow(g_target);
}

static bool ForegroundIsTargetOrOverlay()
{
    HWND fg = GetForegroundWindow();
    if (!fg)              return true;
    if (fg == g_hwnd)     return true;

    DWORD fgPid = 0;
    GetWindowThreadProcessId(fg, &fgPid);
    if (fgPid == GetCurrentProcessId()) return true;

    if (g_target && IsWindow(g_target))
    {
        DWORD targetPid = 0;
        GetWindowThreadProcessId(g_target, &targetPid);
        if (targetPid) g_targetPid = targetPid;
        if (fg == g_target || IsChild(g_target, fg) || GetAncestor(fg, GA_ROOT) == g_target)
            return true;
    }
    if (g_targetPid && fgPid == g_targetPid) return true;

    RefreshTargetPids();
    for (DWORD pid : g_targetPids)
        if (pid && fgPid == pid) return true;
    return false;
}

static void HideMenuLayer()
{
    if (g_uiHwnd && IsWindowVisible(g_uiHwnd))
        ShowWindow(g_uiHwnd, SW_HIDE);
}

static void HideFpsLayer()
{
    if (g_fpsHwnd && IsWindowVisible(g_fpsHwnd))
        ShowWindow(g_fpsHwnd, SW_HIDE);
}

static void HideUiLayer()
{
    HideMenuLayer();
    HideFpsLayer();
}

static void UpdateForegroundVisibility()
{
    if (g_desktop || !g_hwnd) return;

    bool shouldShow = g_overlayEnabled && !g_resyncing &&
        ForegroundIsTargetOrOverlay();
    if (!shouldShow)
    {
        if (!g_overlayHiddenForForeground)
        {
            ShowWindow(g_hwnd, SW_HIDE);
            HideUiLayer();
            SetWindowPos(g_hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            g_overlayHiddenForForeground = true;
        }
        return;
    }
    if (g_overlayHiddenForForeground || !IsWindowVisible(g_hwnd))
    {
        ShowWindow(g_hwnd, SW_SHOWNA);
        g_overlayHiddenForForeground = false;
        g_lastRect = { 0,0,0,0 };
    }
}

static DWORD ExStyle()
{

    DWORD e = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED;
    if (!g_menu && !g_reshadeInput)
        e |= WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
    return e;
}

static DWORD UiExStyle()
{

    return WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED |
        WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
}

static void ApplyWindowMode()
{
    if (!g_hwnd) return;
    if (!g_overlayEnabled)
    {
        ShowWindow(g_hwnd, SW_HIDE);
        HideUiLayer();
        return;
    }
    SetWindowLongPtrA(g_hwnd, GWL_EXSTYLE, ExStyle());
    if (g_uiHwnd)
        SetWindowLongPtrA(g_uiHwnd, GWL_EXSTYLE, UiExStyle());
    if (g_fpsHwnd)
        SetWindowLongPtrA(g_fpsHwnd, GWL_EXSTYLE, UiExStyle());

    if (g_transparent && !g_menu && !g_reshadeInput)
        SetLayeredWindowAttributes(g_hwnd, RGB(0, 0, 0), 0, LWA_COLORKEY);
    else
        SetLayeredWindowAttributes(g_hwnd, 0, 255, LWA_ALPHA);

    SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED | SWP_SHOWWINDOW |
        ((g_menu || g_reshadeInput) ? 0 : SWP_NOACTIVATE));
    if (g_uiHwnd && g_overlayEnabled && g_menu && !g_reshadeInput)
        SetWindowPos(g_uiHwnd, HWND_TOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED | SWP_SHOWWINDOW |
            SWP_NOACTIVATE);
    else
        HideMenuLayer();
    if (g_fpsHwnd && g_overlayEnabled && g_showFPS && !g_reshadeInput)
        SetWindowPos(g_fpsHwnd, HWND_TOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED | SWP_SHOWWINDOW |
            SWP_NOACTIVATE);
    else
        HideFpsLayer();

    if (!g_menu && !g_reshadeInput) FocusTarget();
}

static void SetMenu(bool v)
{

    if (v && !g_overlayEnabled) return;
    if (v && g_showKeyHelp)
    {
        g_showKeyHelp = false;
        SaveSettings();
    }
    g_menu = v;
    if (v) g_reshadeInput = false;
    if (!v) HideMenuLayer();
    ApplyWindowMode();

    if (v) { if (g_hwnd) { SetForegroundWindow(g_hwnd); SetFocus(g_hwnd); } }
    else FocusTarget();
}

static LRESULT CALLBACK UiWndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{

    if (m == WM_DESTROY) { g_running = false; PostQuitMessage(0); return 0; }
    return DefWindowProcA(h, m, w, l);
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (g_menu && ImGui_ImplWin32_WndProcHandler(h, m, w, l))
        return TRUE;
    if (m == WM_SIZE && w != SIZE_MINIMIZED && g_swap && g_ctx)
    {
        const UINT width = LOWORD(l);
        const UINT height = HIWORD(l);
        if (width && height) Resize(width, height);
        return 0;
    }
    if (m == WM_ERASEBKGND) return 1;
    if (m == WM_DESTROY) { g_running = false; PostQuitMessage(0); return 0; }
    return DefWindowProcA(h, m, w, l);
}

static bool InitWindow()
{
    if (!g_desktop)
    {
        g_target = FindTarget();
        if (!g_target)
        {
            Log("[Window] Roblox process/window not found; closing overlay");
            return false;
        }
        char exe[MAX_PATH] = {};
        QueryProcessExeName(g_targetPid, exe, sizeof(exe));
        g_targetExeName = exe;
        Log("[Window] target: %s (pid %lu)", exe[0] ? exe : "?", g_targetPid);
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

    WNDCLASSEXA wcu = {};
    wcu.cbSize = sizeof(wcu);
    wcu.lpfnWndProc = UiWndProc;
    wcu.hInstance = wc.hInstance;
    wcu.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcu.lpszClassName = "FixedHookStatusPipeOverlayUi";
    RegisterClassExA(&wcu);

    g_uiHwnd = CreateWindowExA(UiExStyle(), wcu.lpszClassName, "FeatureModule Overlay UI",
        WS_POPUP, r.left, r.top, g_width, g_height,
        nullptr, nullptr, wcu.hInstance, nullptr);
    if (!g_uiHwnd) return false;

    g_fpsHwnd = CreateWindowExA(UiExStyle(), wcu.lpszClassName, "FeatureModule FPS",
        WS_POPUP, r.left + g_fpsOffsetX, r.top + g_fpsOffsetY, 72, 30,
        nullptr, nullptr, wcu.hInstance, nullptr);
    if (!g_fpsHwnd) return false;

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
    ID3DBlob* b = nullptr;
    if (Compile(VS_SRC, "vs_5_0", &b)) { g_dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_vs); b->Release(); }
    if (Compile(PS_COLOR, "ps_5_0", &b)) { g_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_psColor); b->Release(); }
    if (Compile(PS_DEPTH, "ps_5_0", &b)) { g_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_psDepth); b->Release(); }
    if (Compile(PS_COLOR_DEPTH, "ps_5_0", &b)) { g_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_psColorDepth); b->Release(); }
    if (Compile(PS_DEPTH_VIEW, "ps_5_0", &b)) { g_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_psDepthView); b->Release(); }
    if (Compile(PS_PREMULT, "ps_5_0", &b)) { g_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_psPremult); b->Release(); }

    D3D11_BUFFER_DESC uiCbd = {};
    uiCbd.ByteWidth = sizeof(RemapCB);
    uiCbd.Usage = D3D11_USAGE_DYNAMIC;
    uiCbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    uiCbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_dev->CreateBuffer(&uiCbd, nullptr, &g_uiCB);

    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = sizeof(RemapCB);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(g_dev->CreateBuffer(&cbd, nullptr, &g_cb)) ||
        FAILED(g_dev->CreateBuffer(&cbd, nullptr, &g_cbColor)))
        return false;
    cbd.ByteWidth = sizeof(CombinedRemapCB);
    if (FAILED(g_dev->CreateBuffer(&cbd, nullptr, &g_cbCombined)))
        return false;

    IDXGIDevice1* dxgiDevice1 = nullptr;
    if (SUCCEEDED(g_dev->QueryInterface(__uuidof(IDXGIDevice1), (void**)&dxgiDevice1)))
    {
        dxgiDevice1->SetMaximumFrameLatency(1);
        dxgiDevice1->Release();
    }

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
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = g_imguiIniPath[0] ? g_imguiIniPath : "imgui.ini";
    io.LogFilename = nullptr;
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    ImFontConfig fontConfig;
    fontConfig.OversampleH = 3;
    fontConfig.OversampleV = 2;
    fontConfig.PixelSnapH = false;
    fontConfig.RasterizerMultiply = 1.08f;
    ImFont* uiFont = io.Fonts->AddFontFromFileTTF(
        "C:\\Windows\\Fonts\\segoeui.ttf", 17.0f, &fontConfig,
        io.Fonts->GetGlyphRangesCyrillic());
    if (uiFont) io.FontDefault = uiFont;
    else
    {
        fontConfig.SizePixels = 16.0f;
        io.Fonts->AddFontDefault(&fontConfig);
    }
    ImGui::StyleColorsDark();
    {
        ImGuiStyle& st = ImGui::GetStyle();
        st.Alpha = 1.0f;
        st.WindowRounding = 8.0f;
        st.ChildRounding = 7.0f;
        st.FrameRounding = 6.0f;
        st.GrabRounding = 6.0f;
        st.PopupRounding = 7.0f;
        st.TabRounding = 6.0f;
        st.ScrollbarRounding = 7.0f;
        st.WindowBorderSize = 1.0f;
        st.FrameBorderSize = 0.0f;
        st.WindowPadding = ImVec2(12, 10);
        st.FramePadding = ImVec2(8, 5);
        st.ItemSpacing = ImVec2(7, 6);
        st.ItemInnerSpacing = ImVec2(8, 6);
        st.ScrollbarSize = 11.0f;
        ImVec4* c = st.Colors;
        c[ImGuiCol_WindowBg] = ImVec4(0.045f, 0.048f, 0.052f, 0.95f);
        c[ImGuiCol_ChildBg] = ImVec4(0.060f, 0.064f, 0.070f, 0.96f);
        c[ImGuiCol_PopupBg] = ImVec4(0.055f, 0.058f, 0.064f, 0.98f);
        c[ImGuiCol_Border] = ImVec4(0.24f, 0.25f, 0.28f, 0.85f);
        c[ImGuiCol_Text] = ImVec4(0.94f, 0.95f, 0.97f, 1.0f);
        c[ImGuiCol_TextDisabled] = ImVec4(0.58f, 0.60f, 0.64f, 1.0f);
        c[ImGuiCol_FrameBg] = ImVec4(0.105f, 0.112f, 0.122f, 0.96f);
        c[ImGuiCol_FrameBgHovered] = ImVec4(0.155f, 0.164f, 0.178f, 1.0f);
        c[ImGuiCol_FrameBgActive] = ImVec4(0.195f, 0.205f, 0.222f, 1.0f);
        c[ImGuiCol_TitleBg] = ImVec4(0.055f, 0.058f, 0.063f, 0.97f);
        c[ImGuiCol_TitleBgActive] = ImVec4(0.075f, 0.079f, 0.086f, 0.98f);
        c[ImGuiCol_TitleBgCollapsed] = c[ImGuiCol_TitleBg];
        c[ImGuiCol_MenuBarBg] = c[ImGuiCol_ChildBg];
        c[ImGuiCol_CheckMark] = ImVec4(0.72f, 0.76f, 0.83f, 1.0f);
        c[ImGuiCol_SliderGrab] = ImVec4(0.56f, 0.60f, 0.68f, 1.0f);
        c[ImGuiCol_SliderGrabActive] = ImVec4(0.72f, 0.76f, 0.84f, 1.0f);
        c[ImGuiCol_Button] = ImVec4(0.125f, 0.132f, 0.144f, 0.98f);
        c[ImGuiCol_ButtonHovered] = ImVec4(0.185f, 0.195f, 0.212f, 1.0f);
        c[ImGuiCol_ButtonActive] = ImVec4(0.235f, 0.247f, 0.268f, 1.0f);
        c[ImGuiCol_Header] = c[ImGuiCol_Button];
        c[ImGuiCol_HeaderHovered] = c[ImGuiCol_ButtonHovered];
        c[ImGuiCol_HeaderActive] = c[ImGuiCol_ButtonActive];
        c[ImGuiCol_Tab] = ImVec4(0.090f, 0.095f, 0.104f, 0.98f);
        c[ImGuiCol_TabHovered] = ImVec4(0.175f, 0.185f, 0.202f, 1.0f);
        c[ImGuiCol_TabActive] = ImVec4(0.145f, 0.154f, 0.168f, 1.0f);
        c[ImGuiCol_Separator] = c[ImGuiCol_Border];
        c[ImGuiCol_SeparatorHovered] = c[ImGuiCol_SliderGrab];
        c[ImGuiCol_SeparatorActive] = c[ImGuiCol_SliderGrabActive];
        c[ImGuiCol_PlotLines] = ImVec4(0.68f, 0.72f, 0.80f, 1.0f);
        c[ImGuiCol_PlotHistogram] = ImVec4(0.62f, 0.67f, 0.75f, 1.0f);
        c[ImGuiCol_ScrollbarBg] = ImVec4(0.045f, 0.048f, 0.052f, 0.70f);
        c[ImGuiCol_ScrollbarGrab] = ImVec4(0.25f, 0.26f, 0.29f, 1.0f);
        c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.34f, 0.36f, 0.40f, 1.0f);
        c[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.43f, 0.45f, 0.50f, 1.0f);
        c[ImGuiCol_ResizeGrip] = ImVec4(0.45f, 0.48f, 0.54f, 0.45f);
        c[ImGuiCol_ResizeGripHovered] = c[ImGuiCol_SliderGrab];
        c[ImGuiCol_ResizeGripActive] = c[ImGuiCol_SliderGrabActive];
        c[ImGuiCol_NavHighlight] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);
    return true;
}

static bool CreateUiRtv(ID3D11Texture2D* tex, ID3D11RenderTargetView** out)
{
    D3D11_RENDER_TARGET_VIEW_DESC v = {};
    v.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    v.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    return SUCCEEDED(g_dev->CreateRenderTargetView(tex, &v, out));
}

static bool EnsureUiLayerSize(UINT w, UINT h)
{
    if (!w || !h) return false;
    if (g_uiRT && g_uiRTV && g_uiRTSRV && g_uiW == w && g_uiH == h) return true;

    g_ctx->OMSetRenderTargets(0, nullptr, nullptr);
    SafeRelease(g_uiRTV); SafeRelease(g_uiRTSRV); SafeRelease(g_uiRT);

    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &g_uiRT)))
        return false;

    g_uiRTV = nullptr;
    if (!CreateUiRtv(g_uiRT, &g_uiRTV))
        return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC s = {};
    s.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    s.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    s.Texture2D.MipLevels = 1;
    if (FAILED(g_dev->CreateShaderResourceView(g_uiRT, &s, &g_uiRTSRV)))
        return false;

    g_uiW = w; g_uiH = h;
    Log("[UI] layer render target %ux%u", w, h);
    return true;
}

static bool EnsureUiRegion(UINT w, UINT h)
{
    if (!w || !h) return false;
    if (g_uiRT2 && g_uiRTV2 && g_uiReadbacks[0].texture &&
        g_uiReadbacks[1].texture && g_uiReadbacks[2].texture &&
        g_uiRegionW == w && g_uiRegionH == h)
        return true;

    g_ctx->OMSetRenderTargets(0, nullptr, nullptr);
    SafeRelease(g_uiRTV2); SafeRelease(g_uiRT2);
    ReleaseUiReadbacks();

    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &g_uiRT2)))
        return false;

    g_uiRTV2 = nullptr;
    if (!CreateUiRtv(g_uiRT2, &g_uiRTV2))
        return false;

    D3D11_TEXTURE2D_DESC sd = d;
    sd.BindFlags = 0;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (auto& slot : g_uiReadbacks)
    {
        if (FAILED(g_dev->CreateTexture2D(&sd, nullptr, &slot.texture)))
        {
            ReleaseUiReadbacks();
            return false;
        }
    }

    g_uiRegionW = w; g_uiRegionH = h;
    Log("[UI] region %ux%u", w, h);
    return true;
}

static bool EnsureUiBitmap(UINT w, UINT h)
{
    if (g_uiBitmap && g_uiBitmapW == w && g_uiBitmapH == h)
        return true;

    if (g_uiBitmap)
    {
        if (g_uiDc && g_uiOriginalBitmap) SelectObject(g_uiDc, g_uiOriginalBitmap);
        DeleteObject(g_uiBitmap);
        g_uiBitmap = nullptr;
    }
    g_uiBits = nullptr;

    if (!g_uiDc)
        g_uiDc = CreateCompatibleDC(nullptr);
    if (!g_uiDc) return false;

    BITMAPV5HEADER bh = {};
    bh.bV5Size = sizeof(bh);
    bh.bV5Width = (LONG)w;
    bh.bV5Height = -(LONG)h;
    bh.bV5Planes = 1;
    bh.bV5BitCount = 32;
    bh.bV5Compression = BI_BITFIELDS;
    bh.bV5RedMask = 0x00FF0000;
    bh.bV5GreenMask = 0x0000FF00;
    bh.bV5BlueMask = 0x000000FF;
    bh.bV5AlphaMask = 0xFF000000;

    void* bits = nullptr;
    g_uiBitmap = CreateDIBSection(g_uiDc, (BITMAPINFO*)&bh, DIB_RGB_COLORS,
        &bits, nullptr, 0);
    if (!g_uiBitmap || !bits)
    {
        if (g_uiBitmap) { DeleteObject(g_uiBitmap); g_uiBitmap = nullptr; }
        return false;
    }
    g_uiBits = bits;
    g_uiBitmapW = w;
    g_uiBitmapH = h;
    HGDIOBJ previousBitmap = SelectObject(g_uiDc, g_uiBitmap);
    if (!g_uiOriginalBitmap) g_uiOriginalBitmap = previousBitmap;
    return true;
}

static bool EnsureFpsSurface(UINT w, UINT h)
{
    if (g_fpsBitmap && g_fpsBits) return true;
    if (!g_fpsDc) g_fpsDc = CreateCompatibleDC(nullptr);
    if (!g_fpsDc) return false;

    BITMAPV5HEADER bh = {};
    bh.bV5Size = sizeof(bh);
    bh.bV5Width = (LONG)w;
    bh.bV5Height = -(LONG)h;
    bh.bV5Planes = 1;
    bh.bV5BitCount = 32;
    bh.bV5Compression = BI_BITFIELDS;
    bh.bV5RedMask = 0x00FF0000;
    bh.bV5GreenMask = 0x0000FF00;
    bh.bV5BlueMask = 0x000000FF;
    bh.bV5AlphaMask = 0xFF000000;

    g_fpsBitmap = CreateDIBSection(g_fpsDc, (BITMAPINFO*)&bh,
        DIB_RGB_COLORS, &g_fpsBits, nullptr, 0);
    if (!g_fpsBitmap || !g_fpsBits) return false;
    g_fpsOriginalBitmap = SelectObject(g_fpsDc, g_fpsBitmap);
    g_fpsFont = CreateFontW(-15, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    return true;
}

static void RenderFpsLayer()
{
    constexpr int width = 72;
    constexpr int height = 30;
    constexpr int radius = 7;
    if (!g_fpsHwnd || !g_showFPS || !g_overlayEnabled ||
        g_overlayHiddenForForeground || g_reshadeInput || g_resyncing)
    {
        HideFpsLayer();
        return;
    }
    if (!EnsureFpsSurface(width, height)) return;

    memset(g_fpsBits, 0, width * height * 4);
    RECT rc = { 0, 0, width, height };
    HBRUSH background = CreateSolidBrush(RGB(17, 18, 20));
    FillRect(g_fpsDc, &rc, background);
    DeleteObject(background);

    char text[32];
    if (g_sourceFpsAvailable) snprintf(text, sizeof(text), "%.0f FPS", g_fps);
    else snprintf(text, sizeof(text), "-- FPS");

    HGDIOBJ oldFont = nullptr;
    if (g_fpsFont) oldFont = SelectObject(g_fpsDc, g_fpsFont);
    SetBkMode(g_fpsDc, TRANSPARENT);
    SetTextColor(g_fpsDc, RGB(
        (BYTE)std::clamp((int)lroundf(g_fpsColor[0] * 255.0f), 0, 255),
        (BYTE)std::clamp((int)lroundf(g_fpsColor[1] * 255.0f), 0, 255),
        (BYTE)std::clamp((int)lroundf(g_fpsColor[2] * 255.0f), 0, 255)));
    DrawTextA(g_fpsDc, text, -1, &rc,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (oldFont) SelectObject(g_fpsDc, oldFont);

    DWORD* pixels = (DWORD*)g_fpsBits;
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            const int cx = x < radius ? radius - x :
                (x >= width - radius ? x - (width - radius - 1) : 0);
            const int cy = y < radius ? radius - y :
                (y >= height - radius ? y - (height - radius - 1) : 0);
            DWORD& pixel = pixels[y * width + x];
            if (cx && cy && cx * cx + cy * cy > radius * radius)
            {
                pixel = 0;
                continue;
            }

            const BYTE b = (BYTE)(pixel & 0xff);
            const BYTE g = (BYTE)((pixel >> 8) & 0xff);
            const BYTE r = (BYTE)((pixel >> 16) & 0xff);
            const bool textPixel = r > 32 || g > 32 || b > 32;
            const BYTE alpha = textPixel ? 255 : 210;
            const BYTE outR = (BYTE)((r * alpha + 127) / 255);
            const BYTE outG = (BYTE)((g * alpha + 127) / 255);
            const BYTE outB = (BYTE)((b * alpha + 127) / 255);
            pixel = ((DWORD)alpha << 24) | ((DWORD)outR << 16) |
                ((DWORD)outG << 8) | outB;
        }
    }

    RECT overlayRect = {};
    GetWindowRect(g_hwnd, &overlayRect);

    static bool dragging = false;
    static POINT dragOffset = {};
    POINT cursor = {};
    GetCursorPos(&cursor);
    const bool leftDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    const int currentLeft = overlayRect.left + g_fpsOffsetX;
    const int currentTop = overlayRect.top + g_fpsOffsetY;
    const bool hovered = cursor.x >= currentLeft && cursor.x < currentLeft + width &&
        cursor.y >= currentTop && cursor.y < currentTop + height;

    if (g_menu && leftDown && !dragging && hovered)
    {
        dragging = true;
        dragOffset.x = cursor.x - currentLeft;
        dragOffset.y = cursor.y - currentTop;
    }
    if (dragging)
    {
        if (g_menu && leftDown)
        {
            g_fpsOffsetX = std::clamp(
                (int)(cursor.x - overlayRect.left - dragOffset.x),
                0, std::max(0, (int)g_width - width));
            g_fpsOffsetY = std::clamp(
                (int)(cursor.y - overlayRect.top - dragOffset.y),
                0, std::max(0, (int)g_height - height));
        }
        else
        {
            dragging = false;
            SaveSettings();
        }
    }

    g_fpsOffsetX = std::clamp(g_fpsOffsetX, 0, std::max(0, (int)g_width - width));
    g_fpsOffsetY = std::clamp(g_fpsOffsetY, 0, std::max(0, (int)g_height - height));
    POINT dst = {
        overlayRect.left + g_fpsOffsetX,
        overlayRect.top + g_fpsOffsetY
    };
    POINT src = { 0, 0 };
    SIZE size = { width, height };
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    HDC screenDc = GetDC(nullptr);
    UpdateLayeredWindow(g_fpsHwnd, screenDc, &dst, &size, g_fpsDc,
        &src, 0, &blend, ULW_ALPHA);
    ReleaseDC(nullptr, screenDc);
}

static void SetUiRemap(UINT fullW, UINT fullH, UINT rx, UINT ry, UINT rw, UINT rh)
{
    if (!g_uiCB) return;
    RemapCB cb = {};
    cb.uvScale[0] = fullW ? (float)rw / (float)fullW : 1.0f;
    cb.uvScale[1] = fullH ? (float)rh / (float)fullH : 1.0f;
    cb.uvBias[0] = fullW ? (float)rx / (float)fullW : 0.0f;
    cb.uvBias[1] = fullH ? (float)ry / (float)fullH : 0.0f;

    static RemapCB last = {};
    static bool valid = false;
    if (!valid || memcmp(&last, &cb, sizeof(cb)) != 0)
    {
        D3D11_MAPPED_SUBRESOURCE m = {};
        if (SUCCEEDED(g_ctx->Map(g_uiCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        {
            memcpy(m.pData, &cb, sizeof(cb));
            g_ctx->Unmap(g_uiCB, 0);
            last = cb;
            valid = true;
        }
    }
    g_ctx->PSSetConstantBuffers(0, 1, &g_uiCB);
}

struct UiRegionBounds { int x, y, w, h; };
static bool ComputeUiRegion(float minX, float minY, float maxX, float maxY,
    UINT screenW, UINT screenH, UiRegionBounds& out)
{
    if (!screenW || !screenH || !std::isfinite(minX) || !std::isfinite(minY) ||
        !std::isfinite(maxX) || !std::isfinite(maxY)) return false;

    const float w = (float)screenW, h = (float)screenH;
    const int x = (int)floorf(std::clamp(minX, 0.0f, w));
    const int y = (int)floorf(std::clamp(minY, 0.0f, h));
    const int right = (int)ceilf(std::clamp(maxX, 0.0f, w));
    const int bottom = (int)ceilf(std::clamp(maxY, 0.0f, h));
    if (right <= x || bottom <= y) return false;
    out = { x, y, std::min((right - x + 63) / 64 * 64, (int)screenW - x),
        std::min((bottom - y + 63) / 64 * 64, (int)screenH - y) };
    return true;
}

static void RenderUiLayer(const PipePayload& p)
{
    const bool drawUi = g_menu || g_showKeyHelp;

    if (!g_uiHwnd || !g_overlayEnabled || g_overlayHiddenForForeground ||
        g_reshadeInput || g_resyncing || !drawUi)
    {
        HideMenuLayer();
        return;
    }

    static bool lastMenu = false;
    static bool lastShowKeyHelp = false;
    static bool lastShowFPS = false;
    static UINT lastWidth = 0, lastHeight = 0;
    const bool uiModeChanged = lastMenu != g_menu || lastShowKeyHelp != g_showKeyHelp ||
        lastShowFPS != g_showFPS || lastWidth != g_width || lastHeight != g_height;
    lastShowKeyHelp = g_showKeyHelp;
    if (uiModeChanged)
        for (auto& slot : g_uiReadbacks) slot.pending = false;
    lastMenu = g_menu; lastShowFPS = g_showFPS;
    lastWidth = g_width; lastHeight = g_height;

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    if (g_menu)
    {
        const float maxW = std::max(240.0f, (float)g_width - 16.0f);
        const float maxH = std::max(180.0f, (float)g_height - 16.0f);
        const ImVec2 initial(std::min(413.0f, maxW), std::min(419.0f, maxH));
        const ImVec2 initialPos(
            std::min(1274.0f, std::max(0.0f, (float)g_width - initial.x)),
            std::min(591.0f, std::max(0.0f, (float)g_height - initial.y)));
        ImGui::SetNextWindowSizeConstraints(
            ImVec2(std::min(300.0f, maxW), std::min(200.0f, maxH)), ImVec2(maxW, maxH));
        ImGui::SetNextWindowPos(initialPos,
            g_uiLayoutReset ? ImGuiCond_Always : ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(initial,
            g_uiLayoutReset ? ImGuiCond_Always : ImGuiCond_FirstUseEver);
        if (ImGui::Begin("qwleyshade###qwleyshade_window", &g_menu,
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoNav))
        {
            if (ImGui::BeginTabBar("##main_tabs"))
            {
                if (ImGui::BeginTabItem("Home"))
                {
                    UiSection("SYSTEM");
                    ImGui::TextUnformatted("v1.3.1");
                    ImGui::TextWrapped("%s", OverlayStatus(p));
                    if (g_sourceFpsAvailable)
                        ImGui::TextColored(ImVec4(g_fpsColor[0], g_fpsColor[1], g_fpsColor[2], 1),
                            "%.0f FPS", g_fps);
                    else
                        ImGui::TextDisabled("-- FPS");

                    UiSection("OPTIONS");
                    if (ImGui::Checkbox("Show FPS", &g_showFPS)) SaveSettings();
                    ImGui::ColorEdit3("FPS color", g_fpsColor,
                        ImGuiColorEditFlags_NoInputs);
                    if (ImGui::IsItemDeactivatedAfterEdit()) SaveSettings();

                    ImGui::Checkbox("Depth preview (F3)", &g_depthPreview);

                    UiSection("CONTROL");
                    char overlayKeyLabel[128];
                    if (g_waitKeyTarget == 1)
                        snprintf(overlayKeyLabel, sizeof(overlayKeyLabel),
                            "Hide overlay: [press a key...]");
                    else
                        snprintf(overlayKeyLabel, sizeof(overlayKeyLabel),
                            "Hide overlay: [%s]", VkKeyName(g_minimizeKey));
                    if (ImGui::Button(overlayKeyLabel, ImVec2(-1, 30)))
                    {
                        g_waitKeyTarget = 1;
                        g_waitKeyStart = GetTickCount();
                    }
                    if (g_waitKeyTarget == 1)
                        ImGui::TextDisabled("Escape cancels");
                    if (ImGui::Button("Exit", ImVec2(-1, 32)))
                        g_running = false;
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("GPU"))
                {
                    DrawGpuStatsTab();
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
        }
        ImGui::End();
        if (g_uiLayoutReset) { g_uiLayoutReset = false; SaveSettings(); }
        if (!g_menu && !g_reshadeInput) SetMenu(false);

    }

    if (g_showKeyHelp && !g_menu)
    {
        ImGui::SetNextWindowPos(
            ImVec2((float)g_width * 0.5f, (float)g_height * 0.5f),
            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(295.0f, 0.0f), ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 8.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 4.0f));
        ImGui::PushStyleColor(ImGuiCol_TitleBg, ImVec4(0.24f, 0.25f, 0.26f, 0.98f));
        ImGui::PushStyleColor(ImGuiCol_TitleBgActive, ImVec4(0.24f, 0.25f, 0.26f, 0.98f));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.08f, 0.08f, 0.96f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.18f, 0.18f, 0.18f, 0.90f));

        if (ImGui::Begin("Keybind Help", nullptr,
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav |
            ImGuiWindowFlags_AlwaysAutoResize))
        {
            std::string bindText = MenuBindingText();
            ImGui::TextColored(ImVec4(0.92f, 0.90f, 0.35f, 1.0f),
                "Press %s to open settings", bindText.c_str());
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.78f, 0.78f, 0.78f, 1.0f));
            ImGui::TextWrapped(
                "This popup will disappear after you open the settings menu for the first time.");
            ImGui::PopStyleColor();
        }
        ImGui::End();

        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar(2);
    }

    ImGui::Render();
    if (g_reshadeInput) { HideMenuLayer(); return; }

    ImDrawData* dd = ImGui::GetDrawData();
    if (!dd || dd->TotalVtxCount <= 0) { HideMenuLayer(); return; }

    float minX = FLT_MAX, minY = FLT_MAX, maxX = -FLT_MAX, maxY = -FLT_MAX;
    for (int n = 0; n < dd->CmdListsCount; ++n)
    {
        const ImDrawList* cl = dd->CmdLists[n];
        float listMinX = FLT_MAX, listMinY = FLT_MAX;
        float listMaxX = -FLT_MAX, listMaxY = -FLT_MAX;
        for (int i = 0; i < cl->VtxBuffer.Size; ++i)
        {
            const ImVec2& vertex = cl->VtxBuffer[i].pos;
            listMinX = std::min(listMinX, vertex.x); listMinY = std::min(listMinY, vertex.y);
            listMaxX = std::max(listMaxX, vertex.x); listMaxY = std::max(listMaxY, vertex.y);
        }
        for (int i = 0; i < cl->CmdBuffer.Size; ++i)
        {
            const ImDrawCmd& cmd = cl->CmdBuffer[i];
            if (!cmd.ElemCount) continue;
            const float x0 = std::max(listMinX, cmd.ClipRect.x);
            const float y0 = std::max(listMinY, cmd.ClipRect.y);
            const float x1 = std::min(listMaxX, cmd.ClipRect.z);
            const float y1 = std::min(listMaxY, cmd.ClipRect.w);
            if (x0 >= x1 || y0 >= y1) continue;
            minX = std::min(minX, x0); minY = std::min(minY, y0);
            maxX = std::max(maxX, x1); maxY = std::max(maxY, y1);
        }
    }
    if (minX > maxX || minY > maxY) return;

    UiRegionBounds region = {};
    if (!ComputeUiRegion(minX, minY, maxX, maxY, g_width, g_height, region))
    {
        HideMenuLayer(); return;
    }
    int rx = region.x, ry = region.y, rw = region.w, rh = region.h;
    if (!EnsureUiLayerSize((UINT)rw, (UINT)rh) ||
        !EnsureUiRegion((UINT)rw, (UINT)rh) || !EnsureUiBitmap((UINT)rw, (UINT)rh)) return;

    if (IsWindowVisible(g_uiHwnd))
    {
        RECT overlayRectNow = {};
        GetWindowRect(g_hwnd, &overlayRectNow);
        SetWindowPos(g_uiHwnd, HWND_TOPMOST,
            overlayRectNow.left + rx, overlayRectNow.top + ry, 0, 0,
            SWP_NOSIZE | SWP_NOACTIVATE);
    }

    const ImVec2 savedPos = dd->DisplayPos;
    const ImVec2 savedSize = dd->DisplaySize;
    dd->DisplayPos = ImVec2((float)rx, (float)ry);
    dd->DisplaySize = ImVec2((float)rw, (float)rh);
    ID3D11RenderTargetView* rtv = g_uiRTV;
    const float clear[4] = { 0, 0, 0, 0 };
    g_ctx->ClearRenderTargetView(rtv, clear);
    g_ctx->OMSetDepthStencilState(g_dsOff, 0);
    g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT v = {};
    v.Width = (float)rw; v.Height = (float)rh; v.MaxDepth = 1.0f;
    g_ctx->RSSetViewports(1, &v);
    ImGui_ImplDX11_RenderDrawData(dd);
    dd->DisplayPos = savedPos;
    dd->DisplaySize = savedSize;

    rtv = g_uiRTV2;

    g_ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    g_ctx->RSSetState(nullptr);
    g_ctx->OMSetDepthStencilState(g_dsOff, 0);
    g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
    g_ctx->VSSetShader(g_vs, nullptr, 0);
    g_ctx->PSSetShader(g_psPremult, nullptr, 0);
    SetUiRemap((UINT)rw, (UINT)rh, 0, 0, (UINT)rw, (UINT)rh);
    ID3D11ShaderResourceView* srv = g_uiRTSRV;
    g_ctx->PSSetShaderResources(0, 1, &srv);
    g_ctx->PSSetSamplers(0, 1, &g_sampLinear);
    D3D11_VIEWPORT rv = {};
    rv.Width = (float)rw; rv.Height = (float)rh; rv.MaxDepth = 1.0f;
    g_ctx->RSSetViewports(1, &rv);
    DrawTri();
    ID3D11ShaderResourceView* nullSRV = nullptr;
    g_ctx->PSSetShaderResources(0, 1, &nullSRV);

    UiReadbackSlot* freeSlot = nullptr;
    for (auto& slot : g_uiReadbacks)
        if (!slot.pending) { freeSlot = &slot; break; }
    if (freeSlot)
    {
        g_ctx->CopyResource(freeSlot->texture, g_uiRT2);
        freeSlot->pending = true;
        freeSlot->serial = ++g_uiReadbackSerial;
        freeSlot->x = rx; freeSlot->y = ry;
    }
    UiReadbackSlot* displayed = nullptr;
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    uint64_t upperSerial = UINT64_MAX;
    for (int attempt = 0; attempt < kUiReadbackSlots; ++attempt)
    {
        UiReadbackSlot* candidate = nullptr;
        for (auto& slot : g_uiReadbacks)
        {
            if (slot.pending && slot.serial < upperSerial &&
                (!candidate || slot.serial > candidate->serial))
                candidate = &slot;
        }
        if (!candidate) break;
        upperSerial = candidate->serial;
        const HRESULT hr = g_ctx->Map(candidate->texture, 0, D3D11_MAP_READ,
            D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (SUCCEEDED(hr))
        {
            displayed = candidate;
            break;
        }
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
        {
            continue;
        }
        candidate->pending = false;
    }
    if (!displayed) return;

    const size_t rowBytes = (size_t)rw * 4;
    const BYTE* src = (const BYTE*)mapped.pData;
    BYTE* dst = (BYTE*)g_uiBits;
    if (mapped.RowPitch == rowBytes)
        memcpy(dst, src, rowBytes * (size_t)rh);
    else
        for (int y = 0; y < rh; ++y)
            memcpy(dst + (size_t)y * rowBytes, src + (size_t)y * mapped.RowPitch, rowBytes);
    g_ctx->Unmap(displayed->texture, 0);

    const uint64_t displayedSerial = displayed->serial;
    for (auto& slot : g_uiReadbacks)
        if (slot.pending && slot.serial <= displayedSerial)
            slot.pending = false;

    rx = region.x;
    ry = region.y;

    POINT srcPos = { 0, 0 };
    SIZE winSize = { (LONG)rw, (LONG)rh };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };

    RECT wr = {};
    GetWindowRect(g_hwnd, &wr);
    SetWindowPos(g_uiHwnd, HWND_TOPMOST, wr.left + rx, wr.top + ry,
        rw, rh, SWP_NOACTIVATE | SWP_SHOWWINDOW);

    HDC screenDc = GetDC(g_uiHwnd);
    UpdateLayeredWindow(g_uiHwnd, screenDc, nullptr, &winSize, g_uiDc,
        &srcPos, 0, &bf, ULW_ALPHA);
    ReleaseDC(g_uiHwnd, screenDc);

    g_ctx->OMSetRenderTargets(1, &g_rtv, g_localDSV);
}

static void Follow()
{
    if (g_desktop) return;
    const DWORD now = GetTickCount();
    if (now - g_lastFollow < 16) return;
    g_lastFollow = now;

    static DWORD lastProcessProbe = 0;
    static bool targetProcessDead = false;
    if (now - lastProcessProbe >= 500)
    {
        lastProcessProbe = now;
        targetProcessDead = g_targetPid != 0 && !IsProcessAlive(g_targetPid);
    }
    if (!g_target || !IsWindow(g_target) || targetProcessDead)
    {
        static DWORD lastSearch = 0;
        if (now - lastSearch < 250) return;
        lastSearch = now;

        const DWORD previousPid = g_targetPid;
        HWND found = FindTarget();
        if (!found)
        {
            if (previousPid && IsProcessAlive(previousPid)) return;
            Log("[Window] Roblox process exited; closing overlay");
            g_running = false;
            if (g_hwnd) PostMessageA(g_hwnd, WM_CLOSE, 0, 0);
            return;
        }

        g_target = found;
        targetProcessDead = false;
        char exe[MAX_PATH] = {};
        QueryProcessExeName(g_targetPid, exe, sizeof(exe));
        g_targetExeName = exe;
        g_lastRect = { 0,0,0,0 };
        Log("[Window] reattached to %s (pid %lu)", exe[0] ? exe : "?", g_targetPid);
    }

    UpdateForegroundVisibility();
    RefreshTargetPids();
    if (!g_overlayEnabled || g_overlayHiddenForForeground || g_resyncing) return;

    RECT r = TargetRect();
    if (memcmp(&r, &g_lastRect, sizeof(r)))
    {
        g_lastRect = r;
        SetWindowPos(g_hwnd, HWND_TOPMOST,
            r.left, r.top, r.right - r.left, r.bottom - r.top,
            (g_menu || g_reshadeInput) ? SWP_SHOWWINDOW : (SWP_SHOWWINDOW | SWP_NOACTIVATE));
        if (g_fpsHwnd && IsWindowVisible(g_fpsHwnd))
            SetWindowPos(g_fpsHwnd, HWND_TOPMOST,
                r.left + g_fpsOffsetX, r.top + g_fpsOffsetY, 0, 0,
                SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

static void RequestOverlayResync(const char* reason, DWORD delayMs)
{
    Log("[Resync] requested: %s", reason ? reason : "manual");
    g_menu = false;
    g_reshadeInput = false;
    g_resyncing = true;
    ApplyWindowMode();
    if (g_hwnd) ShowWindow(g_hwnd, SW_HIDE);
    HideUiLayer();
    g_lastRect = { 0,0,0,0 };
    g_resyncDueTick = GetTickCount() + delayMs;
}

static void ForceDepthRebuildTick(const char* reason)
{
    if (!g_ctx || !g_dev) return;

    g_ctx->OMSetRenderTargets(0, nullptr, nullptr);
    SafeRelease(g_localDepthSRV);
    SafeRelease(g_localDSV);
    SafeRelease(g_localDepth);

    const bool ok = g_depthPassEnabled ? CreateLocalDepth(g_width, g_height) : false;

    g_ctx->Flush();

    ++g_depthRecreateCount;
    Log("[AutoColorDepth] depth rebuild #%d (%s) %ux%u ok=%d",
        g_depthRecreateCount, reason ? reason : "auto",
        g_width, g_height, ok ? 1 : 0);
}

static void AutoColorDepthTick()
{
    if (!g_overlayEnabled) { g_autoArmed = false; return; }

    if (!g_autoDepthRebuildEnabled) return;
    if (g_framesPresented < 30 || !g_firstPresentTick) return;

    const DWORD now = GetTickCount();
    const bool pipeLive = g_pipeConnected.load(std::memory_order_acquire) &&
        g_hasPayload.load(std::memory_order_acquire) &&
        g_lastPayloadTick.load(std::memory_order_acquire) &&
        now - g_lastPayloadTick.load(std::memory_order_acquire) <= 750;

    if (!pipeLive)
    {
        g_autoArmed = false;
        return;
    }

    if (!g_autoArmed)
    {
        g_autoArmed = true;
        g_autoAttemptCount = 0;
        g_autoLastAttemptTick = 0;
        g_autoWatchStartTick = now;
        return;
    }

    PipePayload p;
    { std::lock_guard<std::mutex> lk(g_pipeMutex); p = g_payload; }
    const bool colorReady = g_colorSRV && p.colorHandle;
    const bool depthReady = g_depthSRV && p.depthHandle && (p.flags & FLAG_DEPTH_OK);
    if (colorReady && depthReady)
    {
        if (g_autoAttemptCount > 0)
            Log("[AutoColorDepth] color+depth live after %d rebuild(s)",
                g_autoAttemptCount);
        g_autoAttemptCount = 0;
        return;
    }

    if (now - g_autoWatchStartTick < kAutoGraceMs)
        return;

    static const DWORD kAutoRetryGapMs[] = { 800, 1500, 2500, 4000 };
    const int gapCount = (int)(sizeof(kAutoRetryGapMs) / sizeof(kAutoRetryGapMs[0]));
    const DWORD retryGap = kAutoRetryGapMs[std::min(g_autoAttemptCount, gapCount - 1)];
    if (g_autoLastAttemptTick && now - g_autoLastAttemptTick < retryGap)
        return;

    Log("[AutoColorDepth] status incomplete (C=%d D=%d), rebuilding depth (attempt %d/%d)",
        colorReady ? 1 : 0, depthReady ? 1 : 0,
        g_autoAttemptCount + 1, kAutoMaxAttempts);

    ForceDepthRebuildTick("auto color+depth");

    g_autoLastAttemptTick = now;
    ++g_autoAttemptCount;
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
    Resize((UINT)nw, (UINT)nh);
    g_lastRect = r;

    SafeRelease(g_colorSRV); SafeRelease(g_colorTex);
    SafeRelease(g_depthSRV); SafeRelease(g_depthTex);
    g_lastColorHandle = g_lastDepthHandle = 0;
    g_lastColorW = g_lastColorH = g_lastDepthW = g_lastDepthH = 0;
    g_colorTexW = g_colorTexH = g_depthTexW = g_depthTexH = 0;

    ApplyWindowMode();
    g_resyncing = false;
    if (g_overlayEnabled && ForegroundIsTargetOrOverlay())
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

    const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    const bool menuKeyDown = (GetAsyncKeyState(g_menuKey) & 0x8000) != 0;
    const bool menuCombo = menuKeyDown &&
        (g_menuNeedsShift == shift) &&
        (g_menuNeedsCtrl == ctrl) &&
        (g_menuNeedsAlt == alt);
    bool f8 = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    bool minKeyDown = (GetAsyncKeyState(g_minimizeKey) & 0x8000) != 0;
    bool f11 = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;

    if (menuCombo && !pMenu && !g_reshadeInput && g_overlayEnabled)
        SetMenu(!g_menu);

    if (f8 && !pF8)
    {
        g_reshadeInput = !g_reshadeInput;
        if (g_reshadeInput) g_menu = false;
        ApplyWindowMode();

        if (g_reshadeInput)
        {
            HideUiLayer();
            if (g_hwnd)
            {
                SetForegroundWindow(g_hwnd);
                SetFocus(g_hwnd);
            }
        }
    }

    if (minKeyDown && !pF1)
    {
        g_overlayEnabled = !g_overlayEnabled;
        g_menu = false;
        g_reshadeInput = false;
        g_resyncing = false;
        if (!g_overlayEnabled)
        {
            ShowWindow(g_hwnd, SW_HIDE);
            HideUiLayer();
            SetWindowPos(g_hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            FocusTarget();
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

        ForceDepthRebuildTick("manual F11");
    }

    if (GetAsyncKeyState(VK_F3) & 1)
        g_depthPreview = !g_depthPreview;

    if (GetAsyncKeyState(VK_END) & 1)
        g_running = false;

    pMenu = menuCombo; pF8 = f8; pF1 = minKeyDown; pF11 = f11;
}

static std::chrono::steady_clock::time_point AdvanceFrameDeadline(
    std::chrono::steady_clock::time_point deadline,
    std::chrono::steady_clock::duration interval,
    std::chrono::steady_clock::time_point now)
{
    if (interval <= std::chrono::microseconds::zero())
        interval = std::chrono::microseconds(4166);
    deadline += interval;
    if (deadline <= now)
    {

        if (now - deadline > interval * 64)
            deadline = now;
        else
            while (deadline <= now)
                deadline += interval;
    }
    return deadline;
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    using SetDpiAwarenessContextFn = BOOL(WINAPI*)(HANDLE);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    auto setDpiContext = user32 ? reinterpret_cast<SetDpiAwarenessContextFn>(
        GetProcAddress(user32, "SetProcessDpiAwarenessContext")) : nullptr;
    if (!setDpiContext || !setDpiContext((HANDLE)-4))
        SetProcessDPIAware();

    g_console = HasArg("/console");
    g_desktop = HasArg("/desktop");
    g_menu = HasArg("/menu");
    g_transparent = HasArg("/transparent");
    if (HasArg("/no-depth-heuristic")) g_depthHeuristicDraws = 0;
    if (HasArg("/no-auto-depth-rebuild")) g_autoDepthRebuildEnabled = false;
    if (HasArg("/no-vsync")) g_vsync = false;
    if (HasArg("/no-depth")) g_depthPassEnabled = false;
    if (HasArg("/fast-depth-final")) g_fastDepthFinal = true;
    for (const auto& value : ArgValues("profile"))
    {
        if (value == "low") g_requestedPerfTier = PerfTier::Low;
        else if (value == "balanced" || value == "medium") g_requestedPerfTier = PerfTier::Balanced;
        else if (value == "high") g_requestedPerfTier = PerfTier::High;
        else if (value == "auto") g_requestedPerfTier = PerfTier::Auto;
        else continue;
        g_perfTierFromCmdline = true;
    }
    for (const auto& value : ArgValues("overlay-fps"))
    {
        const int fps = atoi(value.c_str());
        if (fps >= 15 && fps <= 500)
        {
            g_maxOverlayFps = (UINT)fps;
            g_overlayFpsExplicit = true;
        }
    }

    g_highResolutionTimer = timeBeginPeriod(1) == TIMERR_NOERROR;
    SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);

    InitTargetList();

    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    char* slash = strrchr(exe, '\\');
    if (slash)
    {
        *(slash + 1) = '\0';
        snprintf(g_iniPath, MAX_PATH, "%soverlay_settings.ini", exe);
        snprintf(g_reshadeIniPath, MAX_PATH, "%sReShade.ini", exe);
        snprintf(g_imguiIniPath, MAX_PATH, "%soverlay_windows.ini", exe);
    }

    LoadSettings();

    SaveSettings();
    WriteReShadeConfig();

    if (g_console)
    {
        AllocConsole();
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    }
    Log("FeatureModule Overlay starting (menu=%s, minimizeKey=%d=%s, reversedDepth=%d)",
        MenuBindingText().c_str(), g_minimizeKey, VkKeyName(g_minimizeKey), (int)g_depthReversed);
    Log("[Target] watching: %s", g_targetExeDisplay.c_str());

    if (!InitWindow()) { Log("[Window] init failed"); return 1; }
    if (!InitD3D()) { Log("[D3D] init failed");    return 2; }

    g_pipeThread = std::thread(PipeThread);

    bool pipeWasSeen = false;
    {
        std::unique_lock<std::mutex> lk(g_pipeMutex);
        g_pipeCv.wait_for(lk, std::chrono::seconds(15), []
            {
                return g_pipeEverConnected.load(std::memory_order_acquire) ||
                    g_pipeStop.load(std::memory_order_acquire);
            });
        pipeWasSeen = g_pipeEverConnected.load(std::memory_order_acquire);
    }
    if (!pipeWasSeen && !g_pipeStop.load(std::memory_order_acquire))
    {
        ShowWindow(g_hwnd, SW_HIDE);
        HideUiLayer();
        MessageBoxA(nullptr, "pls repeat the injection.", "didn't inject",
            MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
        g_running = false;
    }

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
        AutoColorDepthTick();
        Follow();
        ProcessOverlayResync();
        if (g_overlayEnabled && !g_overlayHiddenForForeground && !g_resyncing)
        {
            using FrameClock = std::chrono::steady_clock;
            static uint64_t lastRenderedPacket = UINT64_MAX;
            static auto nextFrameTime = FrameClock::now();
            static auto nextUiTime = FrameClock::now();

            const uint64_t packet = g_sourcePacketSerial.load(std::memory_order_acquire);
            const auto now = FrameClock::now();
            const auto interval = std::chrono::nanoseconds(
                1000000000ll / (long long)std::max<UINT>(30, g_maxOverlayFps));
            const bool menuUi = g_menu || g_reshadeInput;
            const bool wantsUi = menuUi || g_showKeyHelp || g_showFPS;

            static POINT lastCursor = {};
            static DWORD lastInteractionTick = 0;
            static bool wasInteractive = false;
            POINT cursor = {};
            GetCursorPos(&cursor);
            const bool pointerMoved = cursor.x != lastCursor.x || cursor.y != lastCursor.y;
            const bool pointerDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 ||
                (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
            const DWORD uiTick = GetTickCount();
            if (pointerMoved || pointerDown) lastInteractionTick = uiTick;
            lastCursor = cursor;
            const bool interactiveUi = menuUi && uiTick - lastInteractionTick < 400;
            if (interactiveUi && !wasInteractive) nextUiTime = now;
            wasInteractive = interactiveUi;

            const UINT uiFps = menuUi
                ? (interactiveUi ? g_menuActiveFps : g_menuIdleFps)
                : g_counterUiFps;
            const auto uiInterval = std::chrono::nanoseconds(
                1000000000ll / (long long)std::max<UINT>(1, uiFps));
            static bool scheduledMenu = false;
            static bool scheduledShowKeyHelp = false;
            static bool scheduledShowFPS = false;
            const bool frameDue = now >= nextFrameTime;
            const bool uiNeedsFrame = wantsUi && now >= nextUiTime;
            const bool uiModeChanged = scheduledMenu != g_menu ||
                scheduledShowKeyHelp != g_showKeyHelp ||
                scheduledShowFPS != g_showFPS;
            const bool sourceChanged = packet != lastRenderedPacket;

            if ((sourceChanged && frameDue) || uiNeedsFrame || uiModeChanged)
            {
                PipePayload fp;
                { std::lock_guard<std::mutex> lk(g_pipeMutex); fp = g_payload; }
                UpdateSourceFPS();

                EnsureBackBufferMatchesWindow();
                const auto renderBegin = FrameClock::now();
                const bool presentScene = (sourceChanged && frameDue) || g_reshadeInput;
                if (presentScene) Render(fp);

                if (uiNeedsFrame || uiModeChanged)
                {
                    if (g_menu || g_showKeyHelp) RenderUiLayer(fp);
                    else HideMenuLayer();

                    if (g_showFPS) RenderFpsLayer();
                    else HideFpsLayer();
                }

                if (presentScene)
                {
                    RenderDepthFinalPass(fp);
                    g_swap->Present(g_vsync ? 1 : 0, 0);
                    if (!g_framesPresented) g_firstPresentTick = GetTickCount();
                    ++g_framesPresented;
                    lastRenderedPacket = packet;
                }
                else
                {

                    g_ctx->Flush();
                }
                const double renderCostMs = std::chrono::duration<double, std::milli>(
                    FrameClock::now() - renderBegin).count();
                if (presentScene) RecordPerformanceStats(renderCostMs);
                const auto done = FrameClock::now();
                if (presentScene)
                    nextFrameTime = AdvanceFrameDeadline(nextFrameTime, interval, done);
                if (uiNeedsFrame || uiModeChanged)
                    nextUiTime = AdvanceFrameDeadline(nextUiTime, uiInterval, done);
                scheduledMenu = g_menu;
                scheduledShowKeyHelp = g_showKeyHelp;
                scheduledShowFPS = g_showFPS;
            }
            else
            {

                auto wakeAt = frameDue ? now + std::chrono::milliseconds(20) : nextFrameTime;
                if (wantsUi && nextUiTime < wakeAt)
                    wakeAt = nextUiTime;
                if (wakeAt <= now)
                    wakeAt = now + std::chrono::milliseconds(2);
                const auto hardCap = now + std::chrono::milliseconds(20);
                if (wakeAt > hardCap)
                    wakeAt = hardCap;
                std::unique_lock<std::mutex> lk(g_pipeMutex);

                g_pipeCv.wait_until(lk, wakeAt, [packet, frameDue]()
                    {
                        return (frameDue &&
                            g_sourcePacketSerial.load(std::memory_order_acquire) != packet) ||
                            g_pipeStop.load(std::memory_order_acquire);
                    });
            }
        }
        else
        {
            Sleep(16);
        }
    }

    g_pipeStop = true;
    g_pipeCv.notify_all();
    if (g_pipeThread.joinable())
    {
        CancelSynchronousIo((HANDLE)g_pipeThread.native_handle());
        g_pipeThread.join();
    }

    if (g_imguiIniPath[0])
        ImGui::SaveIniSettingsToDisk(g_imguiIniPath);
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
    SafeRelease(g_uiRTV); SafeRelease(g_uiRTSRV); SafeRelease(g_uiRT);
    SafeRelease(g_uiRTV2); SafeRelease(g_uiRT2);
    ReleaseUiReadbacks();
    SafeRelease(g_uiCB);
    SafeRelease(g_psPremult);
    if (g_uiBitmap)
    {
        if (g_uiDc && g_uiOriginalBitmap) SelectObject(g_uiDc, g_uiOriginalBitmap);
        DeleteObject(g_uiBitmap);
        g_uiBitmap = nullptr;
    }
    if (g_uiDc) { DeleteDC(g_uiDc); g_uiDc = nullptr; }
    g_uiBits = nullptr;
    if (g_fpsFont) { DeleteObject(g_fpsFont); g_fpsFont = nullptr; }
    if (g_fpsBitmap)
    {
        if (g_fpsDc && g_fpsOriginalBitmap)
            SelectObject(g_fpsDc, g_fpsOriginalBitmap);
        DeleteObject(g_fpsBitmap);
        g_fpsBitmap = nullptr;
    }
    if (g_fpsDc) { DeleteDC(g_fpsDc); g_fpsDc = nullptr; }
    g_fpsBits = nullptr;
    SafeRelease(g_cbCombined);
    SafeRelease(g_cbColor);
    SafeRelease(g_cb);
    SafeRelease(g_vs);
    SafeRelease(g_psColor);   SafeRelease(g_psDepth);  SafeRelease(g_psColorDepth);
    SafeRelease(g_psDepthView);
    SafeRelease(g_adapter3);
    SafeRelease(g_swap);      SafeRelease(g_ctx);       SafeRelease(g_dev);

    if (g_highResolutionTimer) timeEndPeriod(1);
    if (g_console) FreeConsole();
    return 0;
}

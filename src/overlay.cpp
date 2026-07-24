#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

// === MUST BE FIRST ===
#include <windows.h>
#include <commdlg.h>           // File Open dialog (OPENFILENAMEA)
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

#define PIPE_NAME_W     L"\\\\.\\pipe\\fuckoffmaxey"
#define FLAG_COLOR_OK   (1u << 0)
#define FLAG_DEPTH_OK   (1u << 1)
#define FLAG_FPS_OK     (1u << 2)
#define FLAG_VIEWPORT_OK (1u << 4)
#define PAYLOAD_SIZE    0x40u

#define FORBIDDEN_VK_NONE  0  // ничего не запрещаем

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
static bool   g_console = false;
static bool   g_desktop = false;
static bool   g_running = true;
static bool   g_menu = false;
static bool   g_reshadeInput = false;
static bool   g_invertDepth = false;
static bool   g_transparent = false;
static bool   g_overlayEnabled = true;
static bool   g_overlayHiddenForForeground = false;
static UINT   g_width = 1280, g_height = 720;
static DWORD  g_lastFollow = 0;
static RECT   g_lastRect = { 0,0,0,0 };

static ID3D11Device* g_dev = nullptr;
static ID3D11DeviceContext* g_ctx = nullptr;
static IDXGISwapChain* g_swap = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;
static ID3D11Texture2D* g_localDepth = nullptr;
static ID3D11DepthStencilView* g_localDSV = nullptr;
static ID3D11DepthStencilState* g_dsWrite = nullptr;
static ID3D11DepthStencilState* g_dsOff = nullptr;
static ID3D11SamplerState* g_samp = nullptr;
static ID3D11VertexShader* g_vs = nullptr;
static ID3D11PixelShader* g_psDepth = nullptr;
static ID3D11PixelShader* g_psDepthInv = nullptr;
static ID3D11PixelShader* g_psColor = nullptr;

static ID3D11Texture2D* g_depthTex = nullptr;
static ID3D11ShaderResourceView* g_depthSRV = nullptr;
static ID3D11Texture2D* g_colorTex = nullptr;
static ID3D11ShaderResourceView* g_colorSRV = nullptr;
static uint64_t g_lastDepthHandle = 0, g_lastColorHandle = 0;
static uint32_t g_lastDepthW = 0, g_lastDepthH = 0, g_lastDepthFmt = 0;
static uint32_t g_lastColorW = 0, g_lastColorH = 0, g_lastColorFmt = 0;
static HRESULT  g_lastDepthOpenHR = S_OK, g_lastColorOpenHR = S_OK;
static HRESULT  g_lastDepthSRVHR = S_OK, g_lastColorSRVHR = S_OK;
static int      g_depthHeuristicDraws = 16;
static bool     g_showFPS = true;
static float    g_fpsColor[4] = { 1.0f, 0.95f, 0.70f, 1.0f };
static bool     g_showFpsColorPicker = false;
static int  g_minimizeKey = VK_F1;      // 0x70

static int  g_waitKeyTarget = 0;
static DWORD g_waitKeyStart = 0;
static float g_fps = 0.0f;
static int   g_fpsFrames = 0;
static DWORD g_fpsTick = 0;
static std::string g_gpuName = "Unknown GPU";
static DWORD g_resyncDueTick = 0;

// ====================== RECORDING & PLAYBACK ======================
static bool   g_recording = false;
static bool   g_playback = false;
static std::string g_recordDir;
static int    g_recordFrameIndex = 0;
static DWORD  g_recordStartTick = 0;
static int    g_recordedFrameCount = 0;

static std::vector<std::string> g_playbackFrames;
static int    g_playbackIndex = 0;
static DWORD  g_playbackLastTick = 0;
static float  g_playbackFPS = 30.0f;
static bool   g_playbackLoop = true;
static ID3D11Texture2D* g_playbackColorTex = nullptr;
static ID3D11ShaderResourceView* g_playbackColorSRV = nullptr;
static std::string g_currentPlaybackDir;

// ====================== END RECORD/PLAY ======================

static std::thread       g_pipeThread;
static std::mutex        g_pipeMutex;
static std::atomic<bool> g_pipeStop{ false };
static std::atomic<bool> g_pipeConnected{ false };
static std::atomic<bool> g_hasPayload{ false };
static PipePayload       g_payload;

static char g_iniPath[MAX_PATH] = {};   // overlay_settings.ini
static char g_reshadeIniPath[MAX_PATH] = {};  // ReShade.ini

static void SetMenu(bool v);
static void RequestOverlayResync(const char* reason, DWORD delayMs);
static void ApplyWindowMode();
static void FocusRoblox();
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

static void UpdateFPS()
{
    ++g_fpsFrames;
    DWORD now = GetTickCount();
    if (!g_fpsTick) g_fpsTick = now;
    if (now - g_fpsTick >= 500)
    {
        g_fps = (float)g_fpsFrames * 1000.0f / (float)(now - g_fpsTick);
        g_fpsFrames = 0;
        g_fpsTick = now;
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

static void WriteReShadeOverlayKey()
{
    if (!g_reshadeIniPath[0]) return;

    FILE* rf = fopen(g_reshadeIniPath, "r");
    std::string content;
    if (rf)
    {
        char line[1024];
        while (fgets(line, sizeof(line), rf))
            content += line;
        fclose(rf);
    }

    std::string out;
    bool inInput = false;
    bool keyWritten = false;

    size_t pos = 0;
    while (pos < content.size())
    {
        size_t nl = content.find('\n', pos);
        std::string line = content.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos + 1);
        pos = (nl == std::string::npos) ? content.size() : nl + 1;

        if (!line.empty() && line[0] == '[')
        {
            if (inInput && !keyWritten)
            {
                out += "KeyOverlay=36,0,0,0\n";
                keyWritten = true;
            }
            inInput = (line.find("[INPUT]") != std::string::npos ||
                line.find("[Input]") != std::string::npos ||
                line.find("[input]") != std::string::npos);
        }

        if (inInput && line.find("KeyOverlay=") != std::string::npos)
        {
            out += "KeyOverlay=36,0,0,0\n";
            keyWritten = true;
            continue;
        }

        out += line;
    }

    if (!keyWritten)
    {
        if (!out.empty() && out.back() != '\n')
            out += "\n";
        out += "\n[INPUT]\n";
        out += "KeyOverlay=36,0,0,0\n";
    }

    if (out == content)
        return;

    FILE* wf = fopen(g_reshadeIniPath, "w");
    if (!wf)
    {
        Log("[ReShade] cannot write %s", g_reshadeIniPath);
        return;
    }
    fwrite(out.c_str(), 1, out.size(), wf);
    fclose(wf);
    Log("[ReShade] KeyOverlay=36,0,0,0 written to %s", g_reshadeIniPath);
}

static void LoadSettings()
{
    if (!g_iniPath[0]) return;
    FILE* f = fopen(g_iniPath, "r");
    if (!f) return;

    char  line[256];
    char  section[64] = {};
    while (fgets(line, sizeof(line), f))
    {
        char sec[64];
        if (sscanf(line, "[%63[^]]]", sec) == 1)
        {
            strncpy(section, sec, sizeof(section) - 1);
            continue;
        }

        int   value = 0;
        float r = 0, g = 0, b = 0, a = 0;

        if (_stricmp(section, "Keybinds") == 0)
        {
            if (sscanf(line, "Minimize=%d", &value) == 1)
            {
                if (value > 0 && value < 256)
                    g_minimizeKey = value;
            }
            
        }
        else if (_stricmp(section, "Style") == 0)
        {
            if (sscanf(line, "ShowFPS=%d", &value) == 1)
                g_showFPS = (value != 0);
            else if (sscanf(line, "FpsColor=%f,%f,%f,%f", &r, &g, &b, &a) == 4)
            {
                g_fpsColor[0] = std::max(0.0f, std::min(1.0f, r));
                g_fpsColor[1] = std::max(0.0f, std::min(1.0f, g));
                g_fpsColor[2] = std::max(0.0f, std::min(1.0f, b));
                g_fpsColor[3] = std::max(0.0f, std::min(1.0f, a));
            }
        }
        else
        {
           
            if (sscanf(line, "FpsColor=%f,%f,%f,%f", &r, &g, &b, &a) == 4)
            {
                g_fpsColor[0] = std::max(0.0f, std::min(1.0f, r));
                g_fpsColor[1] = std::max(0.0f, std::min(1.0f, g));
                g_fpsColor[2] = std::max(0.0f, std::min(1.0f, b));
                g_fpsColor[3] = std::max(0.0f, std::min(1.0f, a));
            }
        }
    }
    fclose(f);
}

static void SaveSettings()
{
    if (!g_iniPath[0]) return;
    FILE* f = fopen(g_iniPath, "w");
    if (!f) return;
    fprintf(f, "[Keybinds]\n");
    fprintf(f, "Minimize=%d\n", g_minimizeKey);
    fprintf(f, "\n[Style]\n");
    fprintf(f, "ShowFPS=%d\n", g_showFPS ? 1 : 0);
    fprintf(f, "FpsColor=%.6f,%.6f,%.6f,%.6f\n",
        g_fpsColor[0], g_fpsColor[1], g_fpsColor[2], g_fpsColor[3]);
    fclose(f);

    WriteReShadeOverlayKey();
}

static bool CaptureKeyIfWaiting()
{
    if (!g_waitKeyTarget)
        return false;

    if (GetTickCount() - g_waitKeyStart < 250)
        return true;

    for (int vk = 8; vk < 256; ++vk)
    {
        if (!(GetAsyncKeyState(vk) & 1))
            continue;

        if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT ||
            vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL ||
            vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU ||
            vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON ||
            vk == VK_XBUTTON1 || vk == VK_XBUTTON2)
            continue;

        if (vk == VK_ESCAPE)
        {
            g_waitKeyTarget = 0;
            return true;
        }

        if (g_waitKeyTarget == 1)
            g_minimizeKey = vk;

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

// ====================== BMP SAVER ======================
#pragma pack(push, 1)
struct BMPHeader {
    uint16_t bfType = 0x4D42;      // 'BM'
    uint32_t bfSize = 0;
    uint16_t bfReserved1 = 0;
    uint16_t bfReserved2 = 0;
    uint32_t bfOffBits = 54;
    uint32_t biSize = 40;
    int32_t  biWidth = 0;
    int32_t  biHeight = 0;
    uint16_t biPlanes = 1;
    uint16_t biBitCount = 32;
    uint32_t biCompression = 0;    // BI_RGB
    uint32_t biSizeImage = 0;
    int32_t  biXPelsPerMeter = 0;
    int32_t  biYPelsPerMeter = 0;
    uint32_t biClrUsed = 0;
    uint32_t biClrImportant = 0;
};
#pragma pack(pop)

static bool SaveTextureToBMP(ID3D11Texture2D* tex, const char* filepath)
{
    if (!tex || !g_ctx || !g_dev) return false;

    D3D11_TEXTURE2D_DESC desc;
    tex->GetDesc(&desc);

    // Create staging texture
    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;

    ID3D11Texture2D* staging = nullptr;
    if (FAILED(g_dev->CreateTexture2D(&stagingDesc, nullptr, &staging))) {
        return false;
    }

    g_ctx->CopyResource(staging, tex);

    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(g_ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
        staging->Release();
        return false;
    }

    int w = desc.Width;
    int h = desc.Height;
    int rowPitch = mapped.RowPitch;
    uint8_t* srcData = (uint8_t*)mapped.pData;

    // Create BMP file
    FILE* f = fopen(filepath, "wb");
    if (!f) {
        g_ctx->Unmap(staging, 0);
        staging->Release();
        return false;
    }

    int rowSize = w * 4;
    int padding = (4 - (rowSize % 4)) % 4;
    int imageSize = (rowSize + padding) * h;

    BMPHeader header;
    header.bfSize = sizeof(BMPHeader) + imageSize;
    header.biWidth = w;
    header.biHeight = -h;  // top-down
    header.biSizeImage = imageSize;

    fwrite(&header, sizeof(BMPHeader), 1, f);

    // Write pixels (BGRA -> BGRA, flip if needed)
    std::vector<uint8_t> row(rowSize + padding, 0);
    for (int y = 0; y < h; ++y) {
        uint8_t* srcRow = srcData + y * rowPitch;
        for (int x = 0; x < w; ++x) {
            // BGRA order
            row[x*4 + 0] = srcRow[x*4 + 0];
            row[x*4 + 1] = srcRow[x*4 + 1];
            row[x*4 + 2] = srcRow[x*4 + 2];
            row[x*4 + 3] = 255; // force alpha
        }
        fwrite(row.data(), 1, rowSize + padding, f);
    }

    fclose(f);
    g_ctx->Unmap(staging, 0);
    staging->Release();
    return true;
}
// ====================== END BMP SAVER ======================

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
Texture2D<float4> T:register(t0);
SamplerState S:register(s0);
float4 main(float4 p:SV_POSITION, float2 uv:TEXCOORD0):SV_Target {
    float4 c = T.SampleLevel(S,uv,0);
    return float4(c.rgb,1);
}
)HLSL";

static const char* PS_DEPTH = R"HLSL(
Texture2D<float> T:register(t0);
SamplerState S:register(s0);
float main(float4 p:SV_POSITION, float2 uv:TEXCOORD0):SV_Depth {
    return saturate(T.SampleLevel(S,uv,0).r);
}
)HLSL";

static const char* PS_DEPTH_INV = R"HLSL(
Texture2D<float> T:register(t0);
SamplerState S:register(s0);
float main(float4 p:SV_POSITION, float2 uv:TEXCOORD0):SV_Depth {
    return saturate(1.0 - T.SampleLevel(S,uv,0).r);
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
    SafeRelease(g_localDSV);
    SafeRelease(g_localDepth);
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_D32_FLOAT; d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &g_localDepth))) return false;
    D3D11_DEPTH_STENCIL_VIEW_DESC v = {};
    v.Format = DXGI_FORMAT_D32_FLOAT;
    v.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    return SUCCEEDED(g_dev->CreateDepthStencilView(g_localDepth, &v, &g_localDSV));
}

static bool Resize(UINT w, UINT h)
{
    if (!w || !h || (w == g_width && h == g_height)) return true;
    g_width = w; g_height = h;
    g_ctx->OMSetRenderTargets(0, nullptr, nullptr);
    SafeRelease(g_rtv); SafeRelease(g_localDSV); SafeRelease(g_localDepth);
    HRESULT hr = g_swap->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) { Log("[D3D] ResizeBuffers failed 0x%08lX", (unsigned long)hr); return false; }
    ID3D11Texture2D* bb = nullptr;
    hr = g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb);
    if (FAILED(hr) || !bb) return false;
    g_dev->CreateRenderTargetView(bb, nullptr, &g_rtv);
    bb->Release();
    CreateLocalDepth(w, h);
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

static void UpdateResources(const PipePayload& p)
{
    if (p.colorHandle && p.colorWidth && p.colorHeight)
    {
        bool ch = p.colorHandle != g_lastColorHandle ||
            p.colorWidth != g_lastColorW ||
            p.colorHeight != g_lastColorH ||
            p.colorFormat != g_lastColorFmt;
        if (ch || !g_colorSRV)
        {
            SafeRelease(g_colorSRV); SafeRelease(g_colorTex);
            if (OpenSharedTex(p.colorHandle, &g_colorTex, &g_lastColorOpenHR))
            {
                D3D11_TEXTURE2D_DESC td = {};
                g_colorTex->GetDesc(&td);
                D3D11_SHADER_RESOURCE_VIEW_DESC s = {};
                s.Format = ColorSRVFormat(td.Format);
                s.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                s.Texture2D.MipLevels = 1;
                g_lastColorSRVHR = g_dev->CreateShaderResourceView(g_colorTex, &s, &g_colorSRV);
                Log("[Color] %ux%u fmt=%u srvHR=0x%08lX",
                    p.colorWidth, p.colorHeight, td.Format, (unsigned long)g_lastColorSRVHR);
            }
            else Log("[Color] Open failed 0x%08lX", (unsigned long)g_lastColorOpenHR);
            g_lastColorHandle = p.colorHandle;
            g_lastColorW = p.colorWidth; g_lastColorH = p.colorHeight;
            g_lastColorFmt = p.colorFormat;
            Resize(p.colorWidth, p.colorHeight);
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
            if (OpenSharedTex(p.depthHandle, &g_depthTex, &g_lastDepthOpenHR))
            {
                D3D11_TEXTURE2D_DESC td = {};
                g_depthTex->GetDesc(&td);
                D3D11_SHADER_RESOURCE_VIEW_DESC s = {};
                s.Format = DepthSRVFormat(p.depthFormat ? p.depthFormat : td.Format);
                s.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                s.Texture2D.MipLevels = 1;
                g_lastDepthSRVHR = g_dev->CreateShaderResourceView(g_depthTex, &s, &g_depthSRV);
                Log("[Depth] %ux%u payloadFmt=%u texFmt=%u srvFmt=%u srvHR=0x%08lX",
                    dw, dh, p.depthFormat, td.Format, s.Format, (unsigned long)g_lastDepthSRVHR);
            }
            else Log("[Depth] Open failed 0x%08lX", (unsigned long)g_lastDepthOpenHR);
            g_lastDepthHandle = p.depthHandle;
            g_lastDepthW = dw; g_lastDepthH = dh;
            g_lastDepthFmt = p.depthFormat;
            if (!g_colorSRV) Resize(dw, dh);
        }
    }
}

// ====================== RECORDING FUNCTIONS ======================
static std::string GetTimestamp()
{
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
    localtime_s(&tm, &tt);
    std::stringstream ss;
    ss << std::put_time(&tm, "%Y%m%d_%H%M%S");
    return ss.str();
}

static void StartRecording()
{
    if (g_recording) return;

    std::string ts = GetTimestamp();
    g_recordDir = "recordings\\clip_" + ts;
    std::filesystem::create_directories(g_recordDir);

    g_recordFrameIndex = 0;
    g_recordedFrameCount = 0;
    g_recordStartTick = GetTickCount();
    g_recording = true;

    Log("[Record] Started recording to: %s", g_recordDir.c_str());
}

static void StopRecording()
{
    if (!g_recording) return;
    g_recording = false;
    DWORD duration = GetTickCount() - g_recordStartTick;
    float fps = g_recordedFrameCount > 0 ? (g_recordedFrameCount * 1000.0f / duration) : 0.0f;

    // Save metadata
    std::string metaPath = g_recordDir + "\\meta.txt";
    FILE* mf = fopen(metaPath.c_str(), "w");
    if (mf) {
        fprintf(mf, "width=%u\nheight=%u\nframes=%d\nfps=%.2f\nduration_ms=%u\n",
            g_width, g_height, g_recordedFrameCount, fps, duration);
        fclose(mf);
    }

    Log("[Record] Stopped. Saved %d frames (%.1f fps) to %s", 
        g_recordedFrameCount, fps, g_recordDir.c_str());
}

static void RecordCurrentFrame()
{
    if (!g_recording || !g_colorTex) return;

    char framePath[512];
    snprintf(framePath, sizeof(framePath), "%s\\frame_%05d.bmp", 
             g_recordDir.c_str(), g_recordFrameIndex);

    if (SaveTextureToBMP(g_colorTex, framePath)) {
        g_recordFrameIndex++;
        g_recordedFrameCount++;
    }
}
// ====================== END RECORDING ======================

// ====================== PLAYBACK FUNCTIONS ======================
static void LoadPlaybackFrames(const std::string& dirPath)
{
    g_playbackFrames.clear();
    g_playbackIndex = 0;
    g_currentPlaybackDir = dirPath;

    try {
        for (const auto& entry : std::filesystem::directory_iterator(dirPath)) {
            if (entry.is_regular_file()) {
                std::string ext = entry.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext == ".bmp" || ext == ".png") {
                    g_playbackFrames.push_back(entry.path().string());
                }
            }
        }
    } catch (...) {}

    // Sort numerically
    std::sort(g_playbackFrames.begin(), g_playbackFrames.end());

    Log("[Playback] Loaded %zu frames from %s", g_playbackFrames.size(), dirPath.c_str());
}

static bool LoadPlaybackTexture(const std::string& filepath)
{
    SafeRelease(g_playbackColorSRV);
    SafeRelease(g_playbackColorTex);

    // Load BMP manually (simple loader)
    FILE* f = fopen(filepath.c_str(), "rb");
    if (!f) return false;

    BMPHeader header;
    if (fread(&header, sizeof(BMPHeader), 1, f) != 1 || header.bfType != 0x4D42) {
        fclose(f);
        return false;
    }

    int w = header.biWidth;
    int h = abs(header.biHeight);
    int bpp = header.biBitCount;

    if (bpp != 32 && bpp != 24) {
        fclose(f);
        return false;
    }

    fseek(f, header.bfOffBits, SEEK_SET);

    int rowSize = w * (bpp / 8);
    int padding = (4 - (rowSize % 4)) % 4;
    std::vector<uint8_t> pixels(w * h * 4);

    for (int y = 0; y < h; ++y) {
        int targetY = (header.biHeight > 0) ? (h - 1 - y) : y;
        uint8_t* dst = &pixels[targetY * w * 4];

        if (bpp == 32) {
            fread(dst, 1, rowSize, f);
            fseek(f, padding, SEEK_CUR);
            for (int x = 0; x < w; ++x) {
                // already BGRA
            }
        } else {
            for (int x = 0; x < w; ++x) {
                uint8_t bgr[3];
                fread(bgr, 3, 1, f);
                dst[x*4 + 0] = bgr[0];
                dst[x*4 + 1] = bgr[1];
                dst[x*4 + 2] = bgr[2];
                dst[x*4 + 3] = 255;
            }
            fseek(f, padding, SEEK_CUR);
        }
    }
    fclose(f);

    // Create texture
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA init = {};
    init.pSysMem = pixels.data();
    init.SysMemPitch = w * 4;

    if (FAILED(g_dev->CreateTexture2D(&td, &init, &g_playbackColorTex))) {
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    if (FAILED(g_dev->CreateShaderResourceView(g_playbackColorTex, &srvDesc, &g_playbackColorSRV))) {
        SafeRelease(g_playbackColorTex);
        return false;
    }

    return true;
}

static void StartPlayback(const std::string& dir = "")
{
    if (g_playback) return;

    std::string targetDir = dir.empty() ? g_recordDir : dir;

    if (targetDir.empty() || !std::filesystem::exists(targetDir)) {
        // Try to find the most recent recording
        try {
            std::string latest;
            auto lastWrite = std::filesystem::file_time_type::min();
            for (const auto& entry : std::filesystem::directory_iterator("recordings")) {
                if (entry.is_directory() && entry.path().filename().string().find("clip_") == 0) {
                    auto ftime = std::filesystem::last_write_time(entry);
                    if (ftime > lastWrite) {
                        lastWrite = ftime;
                        latest = entry.path().string();
                    }
                }
            }
            if (!latest.empty()) targetDir = latest;
        } catch (...) {}
    }

    if (targetDir.empty() || !std::filesystem::exists(targetDir)) {
        Log("[Playback] No recording found!");
        return;
    }

    LoadPlaybackFrames(targetDir);

    if (g_playbackFrames.empty()) {
        Log("[Playback] No frames in directory!");
        return;
    }

    g_playback = true;
    g_playbackIndex = 0;
    g_playbackLastTick = GetTickCount();

    // Load first frame
    if (!g_playbackFrames.empty()) {
        LoadPlaybackTexture(g_playbackFrames[0]);
    }

    Log("[Playback] Started playback from %s (%zu frames)", targetDir.c_str(), g_playbackFrames.size());
}

static void StopPlayback()
{
    if (!g_playback) return;
    g_playback = false;
    SafeRelease(g_playbackColorSRV);
    SafeRelease(g_playbackColorTex);
    g_playbackFrames.clear();
    Log("[Playback] Stopped");
}

static void UpdatePlayback()
{
    if (!g_playback || g_playbackFrames.empty()) return;

    DWORD now = GetTickCount();
    float frameTime = 1000.0f / g_playbackFPS;

    if (now - g_playbackLastTick >= (DWORD)frameTime) {
        g_playbackLastTick = now;
        g_playbackIndex++;

        if (g_playbackIndex >= (int)g_playbackFrames.size()) {
            if (g_playbackLoop) {
                g_playbackIndex = 0;
            } else {
                StopPlayback();
                return;
            }
        }

        // Load new frame
        if (g_playbackIndex < (int)g_playbackFrames.size()) {
            LoadPlaybackTexture(g_playbackFrames[g_playbackIndex]);
        }
    }
}
// ====================== END PLAYBACK ======================

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

static void Render()
{
    PipePayload p;
    { std::lock_guard<std::mutex> lk(g_pipeMutex); p = g_payload; }
    UpdateResources(p);

    // Update playback if active
    if (g_playback) {
        UpdatePlayback();
    }

    float clear[4] = { 0,0,0, g_transparent ? 0.0f : 1.0f };
    g_ctx->ClearRenderTargetView(g_rtv, clear);
    ViewportFull();

    // ====================== COLOR PASS (with playback support) ======================
    ID3D11ShaderResourceView* colorSRVToUse = nullptr;

    if (g_playback && g_playbackColorSRV) {
        colorSRVToUse = g_playbackColorSRV;
    } else if (g_colorSRV) {
        colorSRVToUse = g_colorSRV;
    }

    if (colorSRVToUse)
    {
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->OMSetDepthStencilState(g_dsOff, 0);
        g_ctx->VSSetShader(g_vs, nullptr, 0);
        g_ctx->PSSetShader(g_psColor, nullptr, 0);
        g_ctx->PSSetShaderResources(0, 1, &colorSRVToUse);
        g_ctx->PSSetSamplers(0, 1, &g_samp);
        DrawTri();
        ID3D11ShaderResourceView* n = nullptr;
        g_ctx->PSSetShaderResources(0, 1, &n);
    }

    // ====================== DEPTH PASS ======================
    if (g_depthSRV && g_localDSV && !g_playback)   // depth only for live
    {
        g_ctx->ClearDepthStencilView(g_localDSV, D3D11_CLEAR_DEPTH, 1.0f, 0);
        g_ctx->OMSetRenderTargets(1, &g_rtv, g_localDSV);
        g_ctx->OMSetDepthStencilState(g_dsWrite, 0);
        g_ctx->VSSetShader(g_vs, nullptr, 0);
        g_ctx->PSSetShader(g_invertDepth ? g_psDepthInv : g_psDepth, nullptr, 0);
        g_ctx->PSSetShaderResources(0, 1, &g_depthSRV);
        g_ctx->PSSetSamplers(0, 1, &g_samp);
        DrawTri();
        D3D11_VIEWPORT tiny = {};
        tiny.Width = 1; tiny.Height = 1; tiny.MaxDepth = 1;
        g_ctx->RSSetViewports(1, &tiny);
        for (int i = 0; i < g_depthHeuristicDraws; i++) DrawTri();
        ViewportFull();
        ID3D11ShaderResourceView* n = nullptr;
        g_ctx->PSSetShaderResources(0, 1, &n);
    }

    // ====================== RECORDING ======================
    if (g_recording && !g_playback) {
        RecordCurrentFrame();
    }

    // ImGui
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    // FPS overlay
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

    // Menu
    if (g_menu)
    {
        ImGui::SetNextWindowSize(ImVec2(620, 420), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowBgAlpha(0.78f);
        if (ImGui::Begin("\xE2\x96\xBE  Overlay Control", &g_menu,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse))
        {
            ImGui::TextDisabled("SYSTEM STATUS");
            ImGui::Separator();
            ImGui::Text("GPU Device: %s", g_gpuName.c_str());
            ImGui::Text("Status: %s", OverlayStatus(p));

            if (g_recording) {
                ImGui::TextColored(ImVec4(1,0.3f,0.3f,1), "RECORDING: %d frames", g_recordedFrameCount);
            }
            if (g_playback) {
                ImGui::TextColored(ImVec4(0.3f,1,0.5f,1), "PLAYBACK: frame %d/%zu (%.1f fps)", 
                    g_playbackIndex, g_playbackFrames.size(), g_playbackFPS);
            }

            ImGui::Spacing();
            ImGui::TextDisabled("OPTIONS");
            ImGui::Separator();

            if (ImGui::Checkbox("Show FPS", &g_showFPS))
                SaveSettings();

            if (ImGui::Button("FPS counter color", ImVec2(160, 22)))
                g_showFpsColorPicker = !g_showFpsColorPicker;
            ImGui::SameLine();
            ImGui::ColorButton("##fps_col",
                ImVec4(g_fpsColor[0], g_fpsColor[1], g_fpsColor[2], g_fpsColor[3]),
                ImGuiColorEditFlags_NoTooltip, ImVec2(22, 22));
            if (g_showFpsColorPicker)
            {
                if (ImGui::ColorPicker4("##fps_picker", g_fpsColor,
                    ImGuiColorEditFlags_NoSidePreview |
                    ImGuiColorEditFlags_NoSmallPreview |
                    ImGuiColorEditFlags_AlphaBar))
                    SaveSettings();
            }

            ImGui::Spacing();
            ImGui::TextDisabled("KEYBINDS");
            ImGui::Separator();

            // Minimize / overlay toggle key
            char minLabel[128];
            snprintf(minLabel, sizeof(minLabel),
                g_waitKeyTarget == 1
                ? "Overlay On/Off Key: [press key...]"
                : "Overlay On/Off Key: [%s]",
                VkKeyName(g_minimizeKey));
            if (ImGui::Button(minLabel, ImVec2(-1, 22)))
            {
                g_waitKeyTarget = 1;
                g_waitKeyStart = GetTickCount();
                for (int i = 0; i < 256; ++i) GetAsyncKeyState(i);
            }

            if (g_waitKeyTarget)
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.30f, 1.0f),
                    "Press any key. ESC = cancel.");

            // ====================== RECORD / PLAYBACK UI ======================
            ImGui::Spacing();
            ImGui::TextDisabled("VIDEO RECORDING + POST-EFFECT (ReShade Style)");
            ImGui::Separator();

            if (g_recording) {
                if (ImGui::Button("STOP RECORDING", ImVec2(-1, 28))) {
                    StopRecording();
                }
                ImGui::Text("Recording to: %s", g_recordDir.c_str());
                ImGui::Text("Frames: %d", g_recordedFrameCount);
            } else {
                if (ImGui::Button("START RECORD RAW FRAMES", ImVec2(-1, 28))) {
                    StartRecording();
                }
            }

            ImGui::Spacing();

            if (g_playback) {
                if (ImGui::Button("STOP PLAYBACK", ImVec2(-1, 26))) {
                    StopPlayback();
                }
                ImGui::SliderFloat("Playback FPS", &g_playbackFPS, 5.0f, 120.0f, "%.0f");
                ImGui::Checkbox("Loop", &g_playbackLoop);
            } else {
                if (ImGui::Button("PLAY LAST RECORDING", ImVec2(-1, 26))) {
                    StartPlayback();
                }
                if (ImGui::Button("LOAD RECORDING FOLDER...", ImVec2(-1, 22))) {
                    // Simple: open last or show message
                    char path[MAX_PATH] = {};
                    OPENFILENAMEA ofn{};
                    ofn.lStructSize = sizeof(OPENFILENAMEA);
                    ofn.hwndOwner = g_hwnd;
                    ofn.lpstrFilter = "All Files\0*.*\0";
                    ofn.lpstrFile = path;
                    ofn.nMaxFile = MAX_PATH;
                    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
                    if (GetOpenFileNameA(&ofn)) {
                        std::string dir = std::filesystem::path(path).parent_path().string();
                        StartPlayback(dir);
                    }
                }
            }

            ImGui::TextDisabled("Tip: Record raw Roblox -> Apply ReShade effect during playback");
            // ====================== END RECORD/PLAYBACK UI ======================

            ImGui::Separator();
            if (ImGui::Button("Exit Overlay", ImVec2(-1, 24)))
                g_running = false;
        }
        ImGui::End();
    }

    // Sync menu open/close → focus
    static bool lastMenuState = false;
    if (lastMenuState != g_menu)
    {
        ApplyWindowMode();
        if (!g_menu && !g_reshadeInput) FocusRoblox();
        lastMenuState = g_menu;
    }

    ImGui::Render();
    g_ctx->OMSetRenderTargets(1, &g_rtv, g_localDSV);
    g_ctx->OMSetDepthStencilState(g_dsOff, 0);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_ctx->OMSetRenderTargets(1, &g_rtv, g_localDSV);

    g_swap->Present(1, 0);
}

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

static void SetReShadeInput(bool v)
{
    g_reshadeInput = v;
    if (v) g_menu = false;
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
    Compile(VS_SRC, "vs_5_0", &b); g_dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_vs);       b->Release();
    Compile(PS_COLOR, "ps_5_0", &b); g_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_psColor);  b->Release();
    Compile(PS_DEPTH, "ps_5_0", &b); g_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_psDepth);  b->Release();
    Compile(PS_DEPTH_INV, "ps_5_0", &b); g_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_psDepthInv); b->Release();

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
    g_dev->CreateSamplerState(&sp, &g_samp);

    QueryGpuNameFromDevice();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    {
        ImGuiStyle& st = ImGui::GetStyle();
        st.WindowRounding = 3.0f;
        st.FrameRounding = 1.0f;
        st.WindowBorderSize = 1.0f;
        st.FrameBorderSize = 1.0f;
        st.WindowPadding = ImVec2(8, 8);
        st.ItemSpacing = ImVec2(7, 6);
        ImVec4* c = st.Colors;
        c[ImGuiCol_WindowBg] = ImVec4(0.006f, 0.008f, 0.011f, 0.84f);
        c[ImGuiCol_TitleBg] = ImVec4(0.010f, 0.014f, 0.020f, 0.90f);
        c[ImGuiCol_TitleBgActive] = ImVec4(0.018f, 0.026f, 0.036f, 0.94f);
        c[ImGuiCol_Border] = ImVec4(0.16f, 0.20f, 0.24f, 0.70f);
        c[ImGuiCol_FrameBg] = ImVec4(0.015f, 0.028f, 0.048f, 0.92f);
        c[ImGuiCol_FrameBgHovered] = ImVec4(0.035f, 0.080f, 0.140f, 0.95f);
        c[ImGuiCol_CheckMark] = ImVec4(0.92f, 0.90f, 0.65f, 1.0f);
        c[ImGuiCol_Button] = ImVec4(0.025f, 0.025f, 0.030f, 0.88f);
        c[ImGuiCol_ButtonHovered] = ImVec4(0.055f, 0.060f, 0.070f, 0.95f);
        c[ImGuiCol_ButtonActive] = ImVec4(0.085f, 0.090f, 0.100f, 1.00f);
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
    Resize(nw, nh);
    ApplyWindowMode();
    if (g_overlayEnabled && ForegroundIsRobloxOrOverlay())
    {
        ShowWindow(g_hwnd, SW_SHOWNA);
        g_overlayHiddenForForeground = false;
    }
    Log("[Resync] applied: %ux%u at %ld,%ld", nw, nh, r.left, r.top);
}

static void Hotkeys()
{
    if (CaptureKeyIfWaiting())
        return;

    static bool pMenu = false, pF8 = false, pF1 = false, pF11 = false, pF9 = false, pF10 = false;

    bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    bool tabDown = (GetAsyncKeyState(VK_TAB) & 0x8000) != 0;
    bool menuCombo = shift && tabDown;
    bool f8 = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    bool minKeyDown = (GetAsyncKeyState(g_minimizeKey) & 0x8000) != 0;
    bool f11 = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
    bool f9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    bool f10 = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;

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
            ShowWindow(g_hwnd, SW_SHOWNA);
            ApplyWindowMode();
            RequestOverlayResync("minimize key enable/resync", 500);
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
  
    if (GetAsyncKeyState(VK_F2) & 1)
        g_invertDepth = !g_invertDepth;

    if (GetAsyncKeyState(VK_END) & 1)
        g_running = false;

    // RECORDING HOTKEYS
    if (f9 && !pF9)
    {
        if (g_recording) {
            StopRecording();
        } else {
            StartRecording();
        }
    }

    if (f10 && !pF10)
    {
        if (g_playback) {
            StopPlayback();
        } else {
            StartPlayback();
        }
    }

    pMenu = menuCombo; pF8 = f8; pF1 = minKeyDown; pF11 = f11; pF9 = f9; pF10 = f10;
}


int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    g_console = HasArg("/console");
    g_desktop = HasArg("/desktop");
    g_menu = HasArg("/menu");
    g_transparent = HasArg("/transparent");
    if (HasArg("/no-depth-heuristic")) g_depthHeuristicDraws = 0;

    //Пути к файлам настроек
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    char* slash = strrchr(exe, '\\');
    if (slash)
    {
        *(slash + 1) = '\0';
        snprintf(g_iniPath, MAX_PATH, "%soverlay_settings.ini", exe);
        snprintf(g_reshadeIniPath, MAX_PATH, "%sReShade.ini", exe);
    }

    //Загружаем настройки
    LoadSettings();
    WriteReShadeOverlayKey();

    if (g_console)
    {
        AllocConsole();
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    }
    Log("FeatureModule Overlay starting (menu=Shift+Tab, minimizeKey=%d=%s)",
        g_minimizeKey, VkKeyName(g_minimizeKey));

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
            UpdateFPS();
            Render();
        }
        else
        {
            Sleep(10);
        }
        Sleep(1);
    }

    g_pipeStop = true;
    if (g_pipeThread.joinable()) g_pipeThread.join();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    SafeRelease(g_colorSRV);  SafeRelease(g_colorTex);
    SafeRelease(g_depthSRV);  SafeRelease(g_depthTex);
    SafeRelease(g_playbackColorSRV); SafeRelease(g_playbackColorTex);
    SafeRelease(g_rtv);
    SafeRelease(g_localDSV);  SafeRelease(g_localDepth);
    SafeRelease(g_dsWrite);   SafeRelease(g_dsOff);
    SafeRelease(g_samp);
    SafeRelease(g_vs);
    SafeRelease(g_psColor);   SafeRelease(g_psDepth);  SafeRelease(g_psDepthInv);
    SafeRelease(g_swap);      SafeRelease(g_ctx);       SafeRelease(g_dev);

    if (g_console) FreeConsole();
    return 0;
}
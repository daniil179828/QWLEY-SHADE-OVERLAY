#pragma once
// ============================================================
//  imstyle.h  —  Frosted Glass / Glassmorphism Theme
//  Drop-in replacement.  Adds:
//    • Animated menu open / close alpha fade
//    • Hover shimmer on buttons (via DrawList quad)
//    • Animated glowing separators
//    • Smooth FPS pill widget with colour gradient
//    • All helper functions used in DrawMenu block
// ============================================================
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "font.h"
#include <math.h>   // sinf, cosf, fabsf
#include <chrono>

// ──────────────────────────────────────────────────────────────
//  Internal animation state  (all in a single anonymous struct)
// ──────────────────────────────────────────────────────────────
namespace GlassAnim
{
    // Returns seconds since program start (float, wraps ~never in practice)
    inline float Time()
    {
        static auto s_start = std::chrono::steady_clock::now();
        return std::chrono::duration<float>(
            std::chrono::steady_clock::now() - s_start).count();
    }

    // Smooth step from current toward target at rate per-second
    inline float Lerp(float cur, float target, float rate, float dt)
    {
        float diff = target - cur;
        float step = diff * (1.0f - expf(-rate * dt));
        return cur + step;
    }

    // ── per-frame dt helper ──────────────────────────────────
    inline float DeltaTime()
    {
        static float s_prev = Time();
        float now = Time();
        float dt  = now - s_prev;
        s_prev    = now;
        if (dt < 0.0001f) dt = 0.0001f;
        if (dt > 0.1f)    dt = 0.1f;   // clamp for alt-tab spikes
        return dt;
    }

    // ── menu open-alpha animation ────────────────────────────
    struct MenuAnim
    {
        float alpha  = 0.0f;   // 0..1
        bool  wasOpen = false;
    };
    inline MenuAnim& GetMenuAnim() { static MenuAnim s; return s; }

    // Call once per frame before ImGui::Begin for the main menu.
    // Returns the alpha to push via ImGui::SetNextWindowBgAlpha / PushStyleVar.
    inline float TickMenuAlpha(bool menuOpen, float dt)
    {
        MenuAnim& a = GetMenuAnim();
        float target = menuOpen ? 1.0f : 0.0f;
        a.alpha = Lerp(a.alpha, target, 14.0f, dt);
        a.wasOpen = menuOpen;
        return a.alpha;
    }

    // ── button shimmer helper ────────────────────────────────
    //  Call AFTER ImGui::Button() to draw a shimmer overlay on it.
    inline void ButtonShimmer(ImVec2 btnMin, ImVec2 btnMax, bool hovered)
    {
        if (!hovered) return;
        float t = fmodf(Time() * 1.4f, 1.0f);   // 0..1 sweep speed
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // sweep gradient: transparent → white-10% → transparent
        float xLeft  = btnMin.x + (btnMax.x - btnMin.x) * (t - 0.15f);
        float xRight = btnMin.x + (btnMax.x - btnMin.x) * (t + 0.15f);
        xLeft  = xLeft  < btnMin.x ? btnMin.x : xLeft;
        xRight = xRight > btnMax.x ? btnMax.x : xRight;

        dl->PushClipRect(btnMin, btnMax, true);
        dl->AddRectFilledMultiColor(
            ImVec2(xLeft,  btnMin.y), ImVec2(xRight, btnMax.y),
            IM_COL32(255,255,255,0),   IM_COL32(255,255,255,28),
            IM_COL32(255,255,255,28),  IM_COL32(255,255,255,0));
        dl->PopClipRect();
    }

    // ── glowing separator ────────────────────────────────────
    inline void GlowSeparator()
    {
        float t   = Time();
        float pulse = 0.45f + 0.15f * sinf(t * 2.0f);   // 0.30 .. 0.60

        ImVec2 pos = ImGui::GetCursorScreenPos();
        float  w   = ImGui::GetContentRegionAvail().x;
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // base dim line
        dl->AddLine(ImVec2(pos.x,     pos.y),
                    ImVec2(pos.x + w, pos.y),
                    IM_COL32(255,255,255, 18), 1.0f);

        // glowing centre
        ImU32 glow = IM_COL32(200, 210, 255, (ImU32)(pulse * 80));
        dl->AddLine(ImVec2(pos.x + w * 0.15f, pos.y),
                    ImVec2(pos.x + w * 0.85f, pos.y),
                    glow, 1.5f);

        ImGui::Dummy(ImVec2(0, 4));   // reserve vertical space
    }

    // ── section label ────────────────────────────────────────
    inline void SectionLabel(const char* label)
    {
        float t     = Time();
        float wave  = 0.70f + 0.08f * sinf(t * 1.6f);
        ImGui::PushStyleColor(ImGuiCol_Text,
            ImVec4(wave, wave + 0.02f, wave + 0.08f, 1.0f));
        ImGui::TextUnformatted(label);
        ImGui::PopStyleColor();
        GlowSeparator();
    }

    // ── animated FPS pill ────────────────────────────────────
    //  fpsValue: current fps.  Call instead of raw ImGui::Text.
    inline void FpsPill(float fpsValue,
                        const float color[4],
                        float menuAlpha   /*0..1*/)
    {
        // colour: green > 80, yellow 40-80, red < 40
        ImVec4 fpsCol;
        if (fpsValue >= 80.f)
            fpsCol = ImVec4(0.40f, 1.00f, 0.55f, color[3]);
        else if (fpsValue >= 40.f)
            fpsCol = ImVec4(1.00f, 0.85f, 0.25f, color[3]);
        else
            fpsCol = ImVec4(1.00f, 0.32f, 0.32f, color[3]);

        // soft pulse on the alpha
        float t     = Time();
        float pulse = 0.85f + 0.15f * sinf(t * 3.5f);
        fpsCol.w   *= pulse;

        // draw pill background
        ImVec2 pos = ImGui::GetCursorScreenPos();
        char   buf[32];
        snprintf(buf, sizeof(buf), " %.0f fps ", fpsValue);
        ImVec2 sz  = ImGui::CalcTextSize(buf);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        float pad = 4.f;
        ImVec2 r0 = ImVec2(pos.x - pad,        pos.y - pad * 0.5f);
        ImVec2 r1 = ImVec2(pos.x + sz.x + pad, pos.y + sz.y + pad * 0.5f);

        // glass pill bg
        ImU32 bgCol = IM_COL32(10, 10, 18,
            (ImU32)(180 * (1.0f - menuAlpha * 0.3f)));
        dl->AddRectFilled(r0, r1, bgCol, 8.f);
        dl->AddRect(r0, r1, IM_COL32(255,255,255,22), 8.f, 0, 1.0f);

        ImGui::TextColored(fpsCol, "%s", buf);
    }
}

// ──────────────────────────────────────────────────────────────
//  Theme initialisation  (call once after ImGui::CreateContext)
// ──────────────────────────────────────────────────────────────
inline void setImStyleTheme()
{
    ImGuiIO& io = ImGui::GetIO();
    io.MouseDrawCursor = true;

    // font
    ImFontConfig fontCfg;
    fontCfg.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF(
        (void*)rawData, sizeof(rawData), 17.0f, &fontCfg);

    // ── style metrics ──────────────────────────────────────
    ImGuiStyle& s = ImGui::GetStyle();

    s.Alpha                  = 1.0f;
    s.DisabledAlpha          = 0.45f;

    // window
    s.WindowPadding          = ImVec2(14.0f, 12.0f);
    s.WindowRounding         = 14.0f;
    s.WindowBorderSize       = 1.0f;
    s.WindowMinSize          = ImVec2(20.0f, 32.0f);
    s.WindowTitleAlign       = ImVec2(0.5f,  0.5f);
    s.WindowMenuButtonPosition = ImGuiDir_None;

    // child / popup
    s.ChildRounding          = 10.0f;
    s.ChildBorderSize        = 1.0f;
    s.PopupRounding          = 12.0f;
    s.PopupBorderSize        = 1.0f;

    // frame / items
    s.FramePadding           = ImVec2(10.0f,  4.5f);
    s.FrameRounding          = 8.0f;
    s.FrameBorderSize        = 0.0f;
    s.ItemSpacing            = ImVec2(8.0f,   6.0f);
    s.ItemInnerSpacing       = ImVec2(6.0f,   4.0f);
    s.CellPadding            = ImVec2(6.0f,   4.0f);
    s.IndentSpacing          = 16.0f;
    s.ColumnsMinSpacing      = 6.0f;

    // scrollbar / grab
    s.ScrollbarSize          = 8.0f;
    s.ScrollbarRounding      = 8.0f;
    s.GrabMinSize            = 10.0f;
    s.GrabRounding           = 6.0f;

    // tab / button alignment
    s.TabRounding            = 8.0f;
    s.TabBorderSize          = 0.0f;
    s.ColorButtonPosition    = ImGuiDir_Right;
    s.ButtonTextAlign        = ImVec2(0.5f, 0.5f);
    s.SelectableTextAlign    = ImVec2(0.0f, 0.0f);

    // ── colour palette: dark frosted glass ─────────────────
    //  Primary glass layer  rgba(12,13,20, 0.82) ≈ very dark navy
    //  Accent               rgba(120,130,255)     ≈ soft indigo-blue
    //  Border               rgba(255,255,255,0.09)

    constexpr auto C = [](float r, float g, float b, float a = 1.f) {
        return ImVec4(r, g, b, a);
    };

    // glass surface
    const ImVec4 winBg      = C(0.063f, 0.067f, 0.094f, 0.82f);  // frosted navy
    const ImVec4 childBg    = C(0.000f, 0.000f, 0.000f, 0.00f);
    const ImVec4 popupBg    = C(0.055f, 0.058f, 0.082f, 0.92f);
    const ImVec4 frameBg    = C(0.110f, 0.115f, 0.160f, 0.80f);
    const ImVec4 frameBgH   = C(0.150f, 0.155f, 0.210f, 0.85f);
    const ImVec4 frameBgA   = C(0.190f, 0.195f, 0.260f, 0.90f);

    // border: very subtle white rim (glass effect)
    const ImVec4 border     = C(1.0f, 1.0f, 1.0f, 0.09f);
    const ImVec4 borderShadow = C(0.0f, 0.0f, 0.0f, 0.00f);

    // title bar
    const ImVec4 titleBg    = C(0.040f, 0.042f, 0.060f, 1.0f);
    const ImVec4 titleBgAct = C(0.055f, 0.058f, 0.082f, 1.0f);

    // accent: soft indigo-blue
    const ImVec4 accent     = C(0.400f, 0.440f, 1.000f, 1.0f);
    const ImVec4 accentH    = C(0.480f, 0.520f, 1.000f, 1.0f);
    const ImVec4 accentA    = C(0.550f, 0.590f, 1.000f, 1.0f);
    const ImVec4 accentDim  = C(0.400f, 0.440f, 1.000f, 0.25f);

    // buttons: glass-surface style
    const ImVec4 btn        = C(0.160f, 0.165f, 0.225f, 0.80f);
    const ImVec4 btnH       = C(0.220f, 0.228f, 0.310f, 0.90f);
    const ImVec4 btnA       = C(0.280f, 0.290f, 0.390f, 1.00f);

    // text
    const ImVec4 text       = C(0.920f, 0.925f, 0.950f, 1.0f);
    const ImVec4 textDim    = C(0.500f, 0.505f, 0.560f, 1.0f);

    ImVec4* c = s.Colors;
    c[ImGuiCol_Text]                  = text;
    c[ImGuiCol_TextDisabled]          = textDim;
    c[ImGuiCol_WindowBg]              = winBg;
    c[ImGuiCol_ChildBg]               = childBg;
    c[ImGuiCol_PopupBg]               = popupBg;
    c[ImGuiCol_Border]                = border;
    c[ImGuiCol_BorderShadow]          = borderShadow;
    c[ImGuiCol_FrameBg]               = frameBg;
    c[ImGuiCol_FrameBgHovered]        = frameBgH;
    c[ImGuiCol_FrameBgActive]         = frameBgA;
    c[ImGuiCol_TitleBg]               = titleBg;
    c[ImGuiCol_TitleBgActive]         = titleBgAct;
    c[ImGuiCol_TitleBgCollapsed]      = C(0.04f, 0.04f, 0.06f, 0.50f);
    c[ImGuiCol_MenuBarBg]             = C(0.04f, 0.04f, 0.06f, 1.0f);
    c[ImGuiCol_ScrollbarBg]           = C(0.02f, 0.02f, 0.03f, 0.40f);
    c[ImGuiCol_ScrollbarGrab]         = C(0.30f, 0.32f, 0.50f, 0.80f);
    c[ImGuiCol_ScrollbarGrabHovered]  = C(0.40f, 0.43f, 0.65f, 0.90f);
    c[ImGuiCol_ScrollbarGrabActive]   = accent;
    c[ImGuiCol_CheckMark]             = accent;
    c[ImGuiCol_SliderGrab]            = accent;
    c[ImGuiCol_SliderGrabActive]      = accentA;
    c[ImGuiCol_Button]                = btn;
    c[ImGuiCol_ButtonHovered]         = btnH;
    c[ImGuiCol_ButtonActive]          = btnA;
    c[ImGuiCol_Header]                = C(0.20f, 0.21f, 0.30f, 0.60f);
    c[ImGuiCol_HeaderHovered]         = C(0.28f, 0.30f, 0.42f, 0.75f);
    c[ImGuiCol_HeaderActive]          = C(0.35f, 0.37f, 0.52f, 0.90f);
    c[ImGuiCol_Separator]             = C(1.0f, 1.0f, 1.0f, 0.08f);
    c[ImGuiCol_SeparatorHovered]      = C(0.40f, 0.44f, 1.00f, 0.50f);
    c[ImGuiCol_SeparatorActive]       = accent;
    c[ImGuiCol_ResizeGrip]            = accentDim;
    c[ImGuiCol_ResizeGripHovered]     = accentH;
    c[ImGuiCol_ResizeGripActive]      = accentA;
    c[ImGuiCol_Tab]                   = C(0.10f, 0.10f, 0.14f, 1.0f);
    c[ImGuiCol_TabHovered]            = accentH;
    c[ImGuiCol_TabActive]             = accent;
    c[ImGuiCol_TabUnfocused]          = C(0.07f, 0.07f, 0.10f, 1.0f);
    c[ImGuiCol_TabUnfocusedActive]    = C(0.14f, 0.15f, 0.21f, 1.0f);
    c[ImGuiCol_PlotLines]             = accent;
    c[ImGuiCol_PlotLinesHovered]      = accentH;
    c[ImGuiCol_PlotHistogram]         = accent;
    c[ImGuiCol_PlotHistogramHovered]  = accentH;
    c[ImGuiCol_TableHeaderBg]         = C(0.08f, 0.08f, 0.12f, 1.0f);
    c[ImGuiCol_TableBorderStrong]     = C(0.20f, 0.21f, 0.30f, 1.0f);
    c[ImGuiCol_TableBorderLight]      = C(0.14f, 0.15f, 0.21f, 0.6f);
    c[ImGuiCol_TableRowBg]            = C(0.08f, 0.08f, 0.12f, 1.0f);
    c[ImGuiCol_TableRowBgAlt]         = C(0.05f, 0.05f, 0.08f, 1.0f);
    c[ImGuiCol_TextSelectedBg]        = C(accent.x, accent.y, accent.z, 0.35f);
    c[ImGuiCol_DragDropTarget]        = accentH;
    c[ImGuiCol_NavHighlight]          = accentH;
    c[ImGuiCol_NavWindowingHighlight] = C(accent.x, accent.y, accent.z, 0.70f);
    c[ImGuiCol_NavWindowingDimBg]     = C(0.04f, 0.04f, 0.06f, 0.70f);
    c[ImGuiCol_ModalWindowDimBg]      = C(0.04f, 0.04f, 0.06f, 0.60f);
}

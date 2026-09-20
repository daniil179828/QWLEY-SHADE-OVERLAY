
    float _dt         = GlassAnim::DeltaTime();
    float _menuAlpha  = GlassAnim::TickMenuAlpha(g_menu, _dt);

    if (g_showFPS)
    {
        ImGui::SetNextWindowBgAlpha(0.0f);
        ImGui::SetNextWindowPos(ImVec2(12.f, 12.f), ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::Begin("##fps_pill", nullptr,
            ImGuiWindowFlags_NoDecoration    |
            ImGuiWindowFlags_AlwaysAutoResize|
            ImGuiWindowFlags_NoInputs        |
            ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoNav);

        GlassAnim::FpsPill(g_fps, g_fpsColor, _menuAlpha);

        ImGui::End();
        ImGui::PopStyleVar();
    }

    if (_menuAlpha > 0.01f)
    {
       
        float slideY = (1.0f - _menuAlpha) * -22.0f;

        ImGui::SetNextWindowSize(ImVec2(420.0f, 0.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowBgAlpha(
            0.82f * _menuAlpha);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, _menuAlpha);

        float t = GlassAnim::Time();
        float br = 0.40f + 0.12f * sinf(t * 1.1f);
        float bg = 0.44f + 0.10f * sinf(t * 1.3f + 1.0f);
        float bb = 1.00f;
        ImGui::PushStyleColor(ImGuiCol_Border,
            ImVec4(br, bg, bb, 0.35f * _menuAlpha));

        if (slideY != 0.0f)
        {
            ImVec2 cp = ImGui::GetMainViewport()->GetCenter();
            ImGui::SetNextWindowPos(
                ImVec2(cp.x, cp.y + slideY),
                ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        }

        bool menuOpen = g_menu;
        if (ImGui::Begin("  Overlay Control  ", &menuOpen,
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize))
        {
            {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImVec2 wpos   = ImGui::GetWindowPos();
                ImVec2 wsz    = ImGui::GetWindowSize();
                float  ry     = wpos.y + ImGui::GetFrameHeight() + 1.0f;
                dl->AddRectFilledMultiColor(
                    ImVec2(wpos.x,             ry),
                    ImVec2(wpos.x + wsz.x, ry + 1.5f),
                    IM_COL32(100, 115, 255, 80),
                    IM_COL32(100, 115, 255, 80),
                    IM_COL32(100, 115, 255,  0),
                    IM_COL32(100, 115, 255,  0));
            }
            ImGui::Spacing();
            GlassAnim::SectionLabel("SYSTEM STATUS");
            ImGui::Spacing();

            ImGui::Text("GPU   %s", g_gpuName.c_str());
            ImGui::Text("Status  %s", OverlayStatus(p));
            ImGui::Spacing();
            GlassAnim::SectionLabel("OPTIONS");
            ImGui::Spacing();

            ImGui::Checkbox("Depth preview  (F3)", &g_depthPreview);
            {
                bool prevFps = g_showFPS;
                if (ImGui::Checkbox("Show FPS counter", &g_showFPS) && prevFps != g_showFPS)
                    SaveSettings();

                ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - 108.f);
                ImGui::PushItemWidth(80.f);
                if (ImGui::ColorEdit3("##fpsc", g_fpsColor,
                    ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel))
                    SaveSettings();
                ImGui::PopItemWidth();
            }
            ImGui::Spacing();
            GlassAnim::SectionLabel("KEYBINDS");
            ImGui::Spacing();

            {
                char minLabel[160];
                snprintf(minLabel, sizeof(minLabel),
                    g_waitKeyTarget == 1
                        ? "Overlay On/Off  [press any key…]"
                        : "Overlay On/Off  [ %s ]",
                    VkKeyName(g_minimizeKey));
                ImVec2 btnSz(-1, 26);
                ImVec2 btnCursor = ImGui::GetCursorScreenPos();
                bool pressed = ImGui::Button(minLabel, btnSz);
                ImVec2 btnEnd = ImVec2(
                    btnCursor.x + ImGui::GetItemRectSize().x,
                    btnCursor.y + ImGui::GetItemRectSize().y);
                GlassAnim::ButtonShimmer(btnCursor, btnEnd,
                    ImGui::IsItemHovered());
                if (pressed)
                {
                    g_waitKeyTarget = 1;
                    g_waitKeyStart  = GetTickCount();
                }

                if (g_waitKeyTarget)
                {
                    ImVec4 msgCol = g_waitKeyMsg[0]
                        ? ImVec4(1.00f, 0.42f, 0.38f, 1.f)
                        : ImVec4(0.70f, 0.73f, 0.90f, 1.f);
                    ImGui::TextColored(msgCol, "%s",
                        g_waitKeyMsg[0]
                            ? g_waitKeyMsg
                            : "Press any key  (ESC = cancel)");
                }
            }
            ImGui::Spacing();
            GlassAnim::GlowSeparator();
            ImGui::Spacing();

            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                ImVec4(0.75f, 0.18f, 0.18f, 0.90f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                ImVec4(0.90f, 0.22f, 0.22f, 1.00f));

            {
                ImVec2 exitCursor = ImGui::GetCursorScreenPos();
                bool exitPressed  = ImGui::Button("Exit Overlay", ImVec2(-1, 28));
                ImVec2 exitEnd    = ImVec2(
                    exitCursor.x + ImGui::GetItemRectSize().x,
                    exitCursor.y + ImGui::GetItemRectSize().y);
                GlassAnim::ButtonShimmer(exitCursor, exitEnd,
                    ImGui::IsItemHovered());
                if (exitPressed) g_running = false;
            }

            ImGui::PopStyleColor(2);
            ImGui::Spacing();
        }
        ImGui::End();

        ImGui::PopStyleColor();   
        ImGui::PopStyleVar();    
        if (!menuOpen && g_menu)
            SetMenu(false);
    }



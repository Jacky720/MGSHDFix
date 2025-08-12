#pragma once
#include "imgui.h"

namespace MGSHDUI
{
    struct UiState
    {
        // top tabs
        int tabIndex = 0; // Home, Add-ons, Settings, etc.

        // left list + search
        ImGuiTextFilter filter;
        int selectedIndex = -1;

        // example right-panel settings
        bool  enabled = true;
        float vibrance = 0.25f;
        float rgb[3] = { 1.0f, 1.0f, 1.0f };
    };

    UiState& State();

    // Optional: call once after CreateContext if you want the look
    void ApplyTheme();

    // Draw the whole panel (call only when your overlay is visible, before ImGui::Render())
    void Draw();
}

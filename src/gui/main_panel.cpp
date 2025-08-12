#include "common.hpp"
#include "main_panel.hpp"
#include "imgui.h"
#include "logging.hpp"
#include "version.h"

namespace MGSHDUI
{
    // ---------------------------------------------------------------------
    // State
    // ---------------------------------------------------------------------
    static UiState g;
    UiState& State()
    {
        return g;
    }

    // ---------------------------------------------------------------------
    // Window + Tabs
    // ---------------------------------------------------------------------
    static bool BeginMainWindow(const char* title, ImVec2 defaultSize = ImVec2(820, 720))
    {
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_MenuBar;
        ImGui::SetNextWindowSize(defaultSize, ImGuiCond_FirstUseEver);
        return ImGui::Begin(title, nullptr, flags);
    }

    static void DrawTopTabs(int* tabIndex)
    {
        static const char* tabs[] = { "Home", "Add-ons", "Settings", "Statistics", "Log", "About" };
        if (ImGui::BeginTabBar("TopTabs", ImGuiTabBarFlags_Reorderable))
        {
            for (int i = 0; i < IM_ARRAYSIZE(tabs); ++i)
            {
                if (ImGui::BeginTabItem(tabs[i]))
                {
                    *tabIndex = i;
                    ImGui::EndTabItem();
                }
            }
            ImGui::EndTabBar();
        }
    }

    // ---------------------------------------------------------------------
    // Fake data for the left list (replace with your real items)
    // ---------------------------------------------------------------------
    struct Item
    {
        const char* name; const char* file; bool active = false;
    };
    static Item s_items[] = {
        { "SMAA",            "SMAA.fx", false },
        { "Vibrance",        "Vibrance.fx", true },
        { "AdaptiveSharpen", "AdaptiveSharpen.fx", false },
        { "Tonemapper",      "Tonemapper.fx", false },
        { "Bloom",           "Bloom.fx", false },
        { "Vignette",        "Vignette.fx", false },
        { "CAS",             "CAS.fx", false },
    };

    // ---------------------------------------------------------------------
    // Left / Right panes (no Columns used)
    // ---------------------------------------------------------------------
    static void DrawLeftPane(UiState& s, float width = 380.0f)
    {
        ImGui::BeginChild("LeftPane", ImVec2(width, 0), true);

        ImGui::SetNextItemWidth(width - 20.0f);
        s.filter.Draw("Search  ");
        ImGui::Separator();

        if (ImGui::Button("Active to top"))
        { /* TODO: sort active first */
        }
        ImGui::SameLine();
        if (ImGui::Button("Collapse all"))
        { /* TODO: collapse */
        }
        ImGui::Separator();

        ImGui::BeginChild("List", ImVec2(0, 0), false);
        for (int i = 0; i < (int)IM_ARRAYSIZE(s_items); ++i)
        {
            const Item& it = s_items[i];
            char label[256];
            snprintf(label, sizeof(label), "%s  [%s]", it.name, it.file);
            if (!s.filter.PassFilter(label)) continue;

            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth
                | ImGuiTreeNodeFlags_Leaf
                | ImGuiTreeNodeFlags_NoTreePushOnOpen
                | ((s.selectedIndex == i) ? ImGuiTreeNodeFlags_Selected : 0);

            ImGui::TreeNodeEx((void*)(intptr_t)i, flags, "%s", label);
            if (ImGui::IsItemClicked())
                s.selectedIndex = i;
        }
        ImGui::EndChild();

        ImGui::EndChild();
    }

    static void DrawRightPaneHeader()
    {
        if (ImGui::Button("Edit global preprocessor definitions"))
        { /* TODO */
        }
        ImGui::SameLine(); ImGui::TextDisabled("|");
        ImGui::SameLine(); if (ImGui::Button("Reload"))
        { /* TODO */
        }
        ImGui::SameLine(); ImGui::TextDisabled("Performance Mode");
        ImGui::Separator();
    }

    static void DrawRightPaneBody(UiState& s)
    {
        if (s.selectedIndex < 0 || s.selectedIndex >= (int)IM_ARRAYSIZE(s_items))
        {
            ImGui::TextDisabled("Select an item from the list.");
            return;
        }

        const Item& it = s_items[s.selectedIndex];
        ImGui::SeparatorText(it.name);

        if (ImGui::Button("Reset all to default"))
        {
            s.vibrance = 0.25f;
            s.rgb[0] = s.rgb[1] = s.rgb[2] = 1.0f;
            s.enabled = true;
        }
        ImGui::SameLine();
        ImGui::Checkbox("Enabled", &s.enabled);

        if (ImGui::BeginTable("Props", 2, ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 180.0f);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

            auto row = [](const char* label)
                {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted(label);
                    ImGui::TableSetColumnIndex(1);
                };

            row("Vibrance");
            ImGui::SliderFloat("##vibrance", &s.vibrance, 0.0f, 1.0f, "%.3f");

            row("RGB Balance");
            ImGui::PushID("rgb");
            ImGui::SliderFloat("R", &s.rgb[0], 0.0f, 2.0f, "%.3f");
            ImGui::SliderFloat("G", &s.rgb[1], 0.0f, 2.0f, "%.3f");
            ImGui::SliderFloat("B", &s.rgb[2], 0.0f, 2.0f, "%.3f");
            ImGui::PopID();

            ImGui::EndTable();
        }
    }

    static void DrawRightPane(UiState& s)
    {
        ImGui::BeginChild("RightPane", ImVec2(0, 0), true);
        DrawRightPaneHeader();
        DrawRightPaneBody(s);
        ImGui::EndChild();
    }

    // ---------------------------------------------------------------------
    // Tab-specific drawers
    // ---------------------------------------------------------------------
    static void DrawHome()
    {
        ImGui::BeginChild("Home", ImVec2(0, 0), true);
        ImGui::SeparatorText("Home");
        ImGui::TextWrapped("Welcome to MGSHDFix. Pick a tab to get started.");
        ImGui::EndChild();
    }

    static void DrawAddons(UiState& s)
    {
        const float leftWidth = 380.0f;
        DrawLeftPane(s, leftWidth);
        ImGui::SameLine();
        DrawRightPane(s);
    }

    static void DrawSettings()
    {
        ImGui::BeginChild("Settings", ImVec2(0, 0), true);
        ImGui::SeparatorText("Settings");
        ImGui::TextDisabled("Settings go here.");
        ImGui::EndChild();
    }

    static void DrawStatistics()
    {
        ImGui::BeginChild("Stats", ImVec2(0, 0), true);
        ImGui::SeparatorText("Statistics");
        ImGui::TextDisabled("Frame timings, GPU/CPU, etc.");
        ImGui::EndChild();
    }

    static void DrawLog()
    {
        ImGui::BeginChild("Log", ImVec2(0, 0), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
        ImGui::SeparatorText("Log");

        static std::string logContents;
        static std::filesystem::file_time_type lastWriteTime {};
        static size_t lastFileSize = 0;

        try
        {
            // Only re-read if file changed (size or timestamp)
            if (std::filesystem::exists(g_Logging.sLogFile))
            {
                auto ftime = std::filesystem::last_write_time(g_Logging.sLogFile);
                auto fsize = std::filesystem::file_size(g_Logging.sLogFile);

                if (ftime != lastWriteTime || fsize != lastFileSize)
                {
                    std::ifstream file(g_Logging.sLogFile);
                    if (file)
                    {
                        logContents.assign((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());
                        lastWriteTime = ftime;
                        lastFileSize = fsize;
                    }
                }
            }
            else
            {
                logContents = "[Log file not found]";
            }
        }
        catch (const std::exception& e)
        {
            logContents = std::string("[Error reading log file: ") + e.what() + "]";
        }

        // Display log
        ImGui::PushTextWrapPos();
        ImGui::TextUnformatted(logContents.c_str());
        ImGui::PopTextWrapPos();

        // Auto-scroll to bottom
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
            ImGui::SetScrollHereY(1.0f);

        ImGui::EndChild();
    }

    static void DrawAbout()
    {
        ImGui::BeginChild("About", ImVec2(0, 0), true);
        ImGui::SeparatorText("About");
        ImGui::Text("MGSHDFix ");
        ImGui::TextDisabled("Built with Dear ImGui");
        ImGui::Spacing();
        ImGui::TextWrapped("This is a lightweight configuration UI. "
            "Use the Add-ons tab to enable features and tweak parameters.");
        ImGui::EndChild();
    }

    // ---------------------------------------------------------------------
    // Theme
    // ---------------------------------------------------------------------
    void ApplyTheme()
    {
        ImGuiStyle& st = ImGui::GetStyle();
        st.FrameRounding = 3.0f;
        st.GrabRounding = 3.0f;
        st.WindowRounding = 4.0f;
        st.ScrollbarRounding = 6.0f;
        st.FramePadding = ImVec2(8, 5);
        st.ItemSpacing = ImVec2(8, 6);

        ImVec4 accent = ImVec4(0.24f, 0.48f, 0.92f, 1.0f);
        ImVec4 bg = ImVec4(0.08f, 0.08f, 0.10f, 0.95f);

        auto& c = st.Colors;
        c[ImGuiCol_WindowBg] = bg;
        c[ImGuiCol_TitleBg] = ImVec4(bg.x, bg.y, bg.z, 1.0f);
        c[ImGuiCol_TitleBgActive] = ImVec4(bg.x, bg.y, bg.z, 1.0f);
        c[ImGuiCol_Header] = ImVec4(accent.x, accent.y, accent.z, 0.65f);
        c[ImGuiCol_HeaderHovered] = accent;
        c[ImGuiCol_HeaderActive] = accent;
        c[ImGuiCol_Button] = ImVec4(accent.x, accent.y, accent.z, 0.65f);
        c[ImGuiCol_ButtonHovered] = accent;
        c[ImGuiCol_ButtonActive] = accent;
        c[ImGuiCol_SliderGrab] = ImVec4(1, 1, 1, 1);
        c[ImGuiCol_SliderGrabActive] = ImVec4(1, 1, 1, 1);
    }

    // ---------------------------------------------------------------------
    // Main entry
    // ---------------------------------------------------------------------
    void Draw()
    {
        if (!BeginMainWindow("MGSHDFix v" VERSION_STRING))
            return;

        if (ImGui::BeginMenuBar())
        {
            DrawTopTabs(&g.tabIndex);
            ImGui::EndMenuBar();
        }

        switch (g.tabIndex)
        {
        case 0: DrawHome();      break;
        case 1: DrawAddons(g);   break;
        case 2: DrawSettings();  break;
        case 3: DrawStatistics(); break;
        case 4: DrawLog();       break;
        case 5: DrawAbout();     break;
        default: DrawHome();     break;
        }

        ImGui::End();
    }
}

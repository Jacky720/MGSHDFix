#include "common.hpp"
#include "imgui_overlay.hpp"

#include "d3d11_api.hpp"
#include "logging.hpp"
#include "input_handler.hpp"
#include "main_panel.hpp"


#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace
{
    void ShowCursorUntilVisible()
    {
        int v = 0;
        do
        {
            v = ShowCursor(TRUE);
        } while (v < 0);
    }

    void HideCursorUntilHidden()
    {
        int v = 0;
        do
        {
            v = ShowCursor(FALSE);
        } while (v >= 0);
    }

    BOOL s_hadClip = FALSE;
    RECT s_prevClip {};

    SafetyHookInline g_SetCursorPosHook {};
    SafetyHookInline g_ClipCursorHook {};

    using PFN_SetCursorPos = BOOL(WINAPI*)(int, int);
    using PFN_ClipCursor = BOOL(WINAPI*)(const RECT*);

    BOOL WINAPI HookedSetCursorPos(int X, int Y)
    {
        if (g_ImGuiOverlay.IsVisible())
            return TRUE;
        return g_SetCursorPosHook.stdcall<BOOL>(X, Y);
    }

    BOOL WINAPI HookedClipCursor(const RECT* lpRect)
    {
        if (g_ImGuiOverlay.IsVisible())
            return TRUE;
        return g_ClipCursorHook.stdcall<BOOL>(lpRect);
    }

    void InstallUser32HooksOnce()
    {
        static bool installed = false;
        if (installed)
            return;

        HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
        if (!hUser32)
            return;

        if (auto p = reinterpret_cast<void*>(GetProcAddress(hUser32, "SetCursorPos")))
        {
            g_SetCursorPosHook = safetyhook::create_inline(p, reinterpret_cast<void*>(HookedSetCursorPos));
            LOG_HOOK(g_SetCursorPosHook, "SetCursorPos");
        }

        if (auto p = reinterpret_cast<void*>(GetProcAddress(hUser32, "ClipCursor")))
        {
            g_ClipCursorHook = safetyhook::create_inline(p, reinterpret_cast<void*>(HookedClipCursor));
            LOG_HOOK(g_ClipCursorHook, "ClipCursor");
        }

        installed = true;
    }
}

ImGuiOverlay g_ImGuiOverlay;

void ImGuiOverlay::Initialize(IDXGISwapChain* swapChain)
{
    if (m_initialized)
        return;

    if (!g_D3D11Hooks.d3dDevice || !g_D3D11Hooks.d3dDeviceContext)
        return;

    m_hwnd = g_D3D11Hooks.MainHwnd;
    if (!m_hwnd && swapChain)
    {
        DXGI_SWAP_CHAIN_DESC scDesc = {};
        if (SUCCEEDED(swapChain->GetDesc(&scDesc)))
            m_hwnd = scDesc.OutputWindow;
    }
    if (!m_hwnd)
    {
        spdlog::warn("ImGuiOverlay::Initialize: HWND not available.");
        return;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    ImGui::StyleColorsDark();
    MGSHDUI::ApplyTheme();

    if (!ImGui_ImplWin32_Init(m_hwnd))
    {
        spdlog::error("ImGui_ImplWin32_Init failed");
        ImGui::DestroyContext();
        m_hwnd = nullptr;
        return;
    }

    if (!ImGui_ImplDX11_Init(g_D3D11Hooks.d3dDevice.Get(), g_D3D11Hooks.d3dDeviceContext.Get()))
    {
        spdlog::error("ImGui_ImplDX11_Init failed");
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        m_hwnd = nullptr;
        return;
    }

    m_origWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(m_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&ImGuiOverlay::StaticWndProc))
        );
    if (!m_origWndProc)
        spdlog::warn("ImGuiOverlay: failed to subclass WndProc");

    CreateOrRefreshRTV(swapChain);
    InstallUser32HooksOnce();

    g_InputHandler.RegisterHotkey(VK_PRIOR, "ImGui Overlay Toggle", [this]()
        {
            m_visible = !m_visible;

            if (m_visible)
            {
                RECT clip {};
                if (GetClipCursor(&clip))
                {
                    s_hadClip = TRUE;
                    s_prevClip = clip;
                }
                else
                {
                    s_hadClip = FALSE;
                }

                ClipCursor(nullptr);
                SetCapture(nullptr);
                ShowCursorUntilVisible();
            }
            else
            {
                if (s_hadClip)
                    ClipCursor(&s_prevClip);
                else
                    ClipCursor(nullptr);

                HideCursorUntilHidden();
            }
        });

    m_initialized = true;
    spdlog::info("ImGui overlay initialized (HWND=0x{:X})", reinterpret_cast<uintptr_t>(m_hwnd));
}

void ImGuiOverlay::OnPresent(IDXGISwapChain* swapChain)
{
    if (!m_initialized)
        return;

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    ImGui::GetIO().MouseDrawCursor = m_visible;

    if (m_visible)
    {
        MGSHDUI::Draw();

    }

    ImGui::Render();
    CreateOrRefreshRTV(swapChain);

    if (m_rtv && g_D3D11Hooks.d3dDeviceContext)
    {
        ID3D11RenderTargetView* rtv = m_rtv.Get();
        g_D3D11Hooks.d3dDeviceContext->OMSetRenderTargets(1, &rtv, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }

}

void ImGuiOverlay::OnResize()
{
    m_rtv.Reset();
    m_rtvWidth = 0;
    m_rtvHeight = 0;
}

void ImGuiOverlay::Shutdown()
{
    if (!m_initialized)
        return;

    if (m_visible)
    {
        if (s_hadClip)
            ClipCursor(&s_prevClip);
        else
            ClipCursor(nullptr);
        HideCursorUntilHidden();
    }

    OnResize();
    UnsubclassWndProc();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    m_initialized = false;
    m_visible = false;
    m_hwnd = nullptr;
}

void ImGuiOverlay::SetVisible(bool visible)
{
    m_visible = visible;
}

bool ImGuiOverlay::IsVisible() const
{
    return m_visible;
}

void ImGuiOverlay::CreateOrRefreshRTV(IDXGISwapChain* swapChain)
{
    if (!g_D3D11Hooks.d3dDevice || !swapChain)
    {
        m_rtv.Reset();
        m_rtvWidth = 0;
        m_rtvHeight = 0;
        return;
    }

    DXGI_SWAP_CHAIN_DESC scDesc = {};
    if (FAILED(swapChain->GetDesc(&scDesc)))
        return;

    const bool needRecreate = !m_rtv
        || scDesc.BufferDesc.Width != m_rtvWidth
        || scDesc.BufferDesc.Height != m_rtvHeight;

    if (!needRecreate)
        return;

    m_rtv.Reset();

    ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(backBuffer.GetAddressOf()))) || !backBuffer)
        return;

    ComPtr<ID3D11RenderTargetView> rtv;
    if (FAILED(g_D3D11Hooks.d3dDevice->CreateRenderTargetView(backBuffer.Get(), nullptr, rtv.GetAddressOf())))
        return;

    m_rtv = rtv;
    m_rtvWidth = scDesc.BufferDesc.Width;
    m_rtvHeight = scDesc.BufferDesc.Height;
}

void ImGuiOverlay::UnsubclassWndProc()
{
    if (m_hwnd && m_origWndProc)
    {
        SetWindowLongPtrW(m_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(m_origWndProc));
        m_origWndProc = nullptr;
    }
}

LRESULT CALLBACK ImGuiOverlay::StaticWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);

    const bool overlayVisible = g_ImGuiOverlay.IsVisible();
    if (overlayVisible)
    {
        ImGuiIO& io = ImGui::GetIO();

        switch (msg)
        {
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
            if (io.WantCaptureMouse)
                return 1;
            break;

        case WM_INPUT:
            if (io.WantCaptureMouse)
                return 0;
            break;

        case WM_SETCURSOR:
            if (io.WantCaptureMouse)
                return TRUE;
            break;

        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP:
        case WM_CHAR:
            if (io.WantCaptureKeyboard)
                return 1;
            break;
        }
    }

    if (g_ImGuiOverlay.m_origWndProc)
        return CallWindowProcW(g_ImGuiOverlay.m_origWndProc, hWnd, msg, wParam, lParam);

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

#pragma once

#include <wrl/client.h>
#include <d3d11.h>
using Microsoft::WRL::ComPtr;


class ImGuiOverlay
{
public:
    void Initialize(IDXGISwapChain* swapChain);
    void OnPresent(IDXGISwapChain* swapChain);
    void OnResize();
    void Shutdown();

    // Optional if you want to toggle externally
    void SetVisible(bool visible);
    bool IsVisible() const;

private:
    void CreateOrRefreshRTV(IDXGISwapChain* swapChain);
    void InitBackends(IDXGISwapChain* swapChain);
    void UnsubclassWndProc();

    static LRESULT CALLBACK StaticWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

private:
    bool m_initialized = false;
    bool m_visible = false;

    HWND m_hwnd = nullptr;
    WNDPROC m_origWndProc = nullptr;

    ComPtr<ID3D11RenderTargetView> m_rtv;
    UINT m_rtvWidth = 0;
    UINT m_rtvHeight = 0;
};

// Global instance
extern ImGuiOverlay g_ImGuiOverlay;

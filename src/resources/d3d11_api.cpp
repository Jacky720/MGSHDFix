#include "common.hpp"
#include "d3d11_api.hpp"

#include "gpu_check.hpp"
#include "line_scaling.hpp"
#include "logging.hpp"
#include "input_handler.hpp"
#include "imgui_overlay.hpp"

#pragma comment(lib, "d3d11.lib")

void afterPresent();

namespace
{
    // Global hooks (function-impl level, not per-instance vtable)
    SafetyHookInline PresentHookGlobal {};
    SafetyHookInline ResizeBuffersHookGlobal {};

    // We latch the first real swapchain we see
    std::atomic<IDXGISwapChain*> g_FirstSeenSwapchain { nullptr };

    // Utility: create an invisible tiny window for dummy swapchain bootstrap
    static HWND CreateHiddenWindow()
    {
        static const wchar_t* kCls = L"MGSHDFix_DummyDX11Wnd";
        static std::once_flag regOnce;
        std::call_once(regOnce, []
            {
                WNDCLASSEXW wc {};
                wc.cbSize = sizeof(wc);
                wc.style = CS_HREDRAW | CS_VREDRAW;
                wc.lpfnWndProc = DefWindowProcW;
                wc.hInstance = GetModuleHandleW(nullptr);
                wc.hCursor = LoadCursorW(nullptr, IDC_NO);
                wc.lpszClassName = kCls;
                RegisterClassExW(&wc);
            });

        return CreateWindowExW(
            0, kCls, L"", WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, 16, 16,
            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr
        );
    }

    static void OneTimeGpuLog(IDXGISwapChain* swap)
    {
        static std::atomic_bool done { false };
        if (done || !swap)
        {
            return;
        }

        IDXGIDevice* dxgiDevice = nullptr;
        if (SUCCEEDED(swap->GetDevice(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice))) && dxgiDevice)
        {
            IDXGIAdapter* adapter = nullptr;
            if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) && adapter)
            {
                // Assign into ComPtr on g_D3D11Hooks. This AddRefs.
                g_D3D11Hooks.dxgiAdapter = adapter;

                DXGI_ADAPTER_DESC desc {};
                if (SUCCEEDED(adapter->GetDesc(&desc)))
                {
                    const std::string gpuName = Util::WideToUTF8(desc.Description);

                    LARGE_INTEGER driverVersion {};
                    if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driverVersion)))
                    {
                        const UINT product = HIWORD(driverVersion.HighPart);
                        const UINT version = LOWORD(driverVersion.HighPart);
                        const UINT subVersion = HIWORD(driverVersion.LowPart);
                        const UINT build = LOWORD(driverVersion.LowPart);

                        CheckMinimumGPU(gpuName, product, version, subVersion, build);
                    }
                    else
                    {
                        spdlog::warn("DXGI: Could not query GPU driver version.");
                        spdlog::info("DXGI: GPU {}", gpuName);
                    }
                }

                adapter->Release(); // ComPtr keeps its own ref now
            }

            dxgiDevice->Release();
        }

        done = true;
    }

    // Enumerate a top-level window for this process as fallback
    struct EnumCtx
    {
        DWORD pid;
        HWND* out;
    };

    static BOOL CALLBACK EnumTopLevelProc(HWND hwnd, LPARAM lParam)
    {
        auto* ctx = reinterpret_cast<EnumCtx*>(lParam);
        DWORD wpid = 0;
        GetWindowThreadProcessId(hwnd, &wpid);
        if (wpid == ctx->pid && IsWindowVisible(hwnd))
        {
            const LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
            if ((ex & WS_EX_TOOLWINDOW) == 0)
            {
                *ctx->out = hwnd;
                return FALSE; // stop, found one
            }
        }
        return TRUE; // keep going
    }

    static HWND GetSwapchainHWND(IDXGISwapChain* sc)
    {
        if (!sc)
        {
            return nullptr;
        }

        DXGI_SWAP_CHAIN_DESC desc {};
        if (SUCCEEDED(sc->GetDesc(&desc)) && desc.OutputWindow)
        {
            return desc.OutputWindow;
        }

        HWND found = nullptr;
        EnumCtx ctx { GetCurrentProcessId(), &found };
        EnumWindows(EnumTopLevelProc, reinterpret_cast<LPARAM>(&ctx));
        return found;
    }

    static void CaptureFirstSwapchainAndInit(IDXGISwapChain* sc)
    {
        g_D3D11Hooks.swapChain = sc;

        // Device/context
        ComPtr<ID3D11Device> device;
        if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(device.GetAddressOf()))) && device)
        {
            g_D3D11Hooks.d3dDevice = device;

            ComPtr<ID3D11DeviceContext> context;
            device->GetImmediateContext(context.GetAddressOf());
            if (context)
            {
                g_D3D11Hooks.d3dDeviceContext = context;
            }
        }
        else
        {
            spdlog::warn("DXGI: First swapchain has no ID3D11Device.");
        }

        // HWND for ImGui Win32 backend
        HWND hwnd = GetSwapchainHWND(sc);
        g_D3D11Hooks.MainHwnd = hwnd;
        spdlog::info("DXGI: Captured HWND = {}", fmt::ptr(hwnd));

        OneTimeGpuLog(sc);

        // Initialize overlay/backend now that HWND and device/context are set
        afterPresent();
    }

    // Detours (global)
    static HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* pSwapChain, UINT syncInterval, UINT flags)
    {
        if (pSwapChain)
        {
            IDXGISwapChain* expected = nullptr;
            if (g_FirstSeenSwapchain.compare_exchange_strong(expected, pSwapChain))
            {
                CaptureFirstSwapchainAndInit(pSwapChain);
            }

            g_ImGuiOverlay.OnPresent(pSwapChain);
            g_InputHandler.Update();
        }

        return PresentHookGlobal.stdcall<HRESULT>(pSwapChain, syncInterval, flags);
    }

    static HRESULT STDMETHODCALLTYPE HookedResizeBuffers(IDXGISwapChain* swap, UINT Count, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT Flags)
    {
        // Do overlay side first so it can release resources before the real resize
        g_ImGuiOverlay.OnResize();
        return ResizeBuffersHookGlobal.stdcall<HRESULT>(swap, Count, Width, Height, NewFormat, Flags);
    }

    // Bootstrap: create a dummy device/swapchain, read impl addresses, and hook them
    static bool InstallGlobalSwapchainHooksViaDummy()
    {
        if (!GetModuleHandleW(L"dxgi.dll") || !GetModuleHandleW(L"d3d11.dll"))
        {
            spdlog::warn("DXGI/D3D11 not loaded yet; skipping dummy hookup.");
            return false;
        }

        HWND hwnd = CreateHiddenWindow();
        if (!hwnd)
        {
            spdlog::error("Failed to create dummy window for DXGI bootstrap.");
            return false;
        }

        DXGI_SWAP_CHAIN_DESC scd {};
        scd.BufferCount = 2;
        scd.BufferDesc.Width = 16;
        scd.BufferDesc.Height = 16;
        scd.BufferDesc.RefreshRate.Numerator = 60;
        scd.BufferDesc.RefreshRate.Denominator = 1;
        scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd.OutputWindow = hwnd;
        scd.SampleDesc.Count = 1;
        scd.SampleDesc.Quality = 0;
        scd.Windowed = TRUE;
        scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        scd.Flags = 0;

        UINT createFlags = 0;
        // createFlags |= D3D11_CREATE_DEVICE_DEBUG;

        constexpr D3D_FEATURE_LEVEL kFeatureLevels[] =
        {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0
        };

        ComPtr<ID3D11Device>        dev;
        ComPtr<ID3D11DeviceContext> ctx;
        ComPtr<IDXGISwapChain>      sc;

        D3D_FEATURE_LEVEL obtained {};
        const HRESULT hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createFlags,
            kFeatureLevels, _countof(kFeatureLevels), D3D11_SDK_VERSION,
            &scd, sc.GetAddressOf(), dev.GetAddressOf(), &obtained, ctx.GetAddressOf()
        );

        if (FAILED(hr) || !sc)
        {
            DestroyWindow(hwnd);
            spdlog::error("Dummy D3D11CreateDeviceAndSwapChain failed. hr=0x{:08X}",
                static_cast<unsigned int>(hr));
            return false;
        }

        // Extract implementation addresses
        void** vt = *reinterpret_cast<void***>(sc.Get());
        if (!vt)
        {
            sc.Reset(); ctx.Reset(); dev.Reset();
            DestroyWindow(hwnd);
            spdlog::error("Dummy swapchain has null vtable.");
            return false;
        }

        constexpr size_t kPresentIndex = 8;   // IDXGISwapChain::Present
        constexpr size_t kResizeBuffersIndex = 13;  // IDXGISwapChain::ResizeBuffers

        void* presentAddr = vt[kPresentIndex];
        void* resizeBuffersAddr = vt[kResizeBuffersIndex];

        spdlog::info("Global swapchain impls: Present={} ResizeBuffers={}",
            fmt::ptr(presentAddr), fmt::ptr(resizeBuffersAddr));

        bool ok = true;

        if (presentAddr && !PresentHookGlobal)
        {
            PresentHookGlobal = safetyhook::create_inline(presentAddr, reinterpret_cast<void*>(HookedPresent));
            LOG_HOOK(PresentHookGlobal, "Global IDXGISwapChain::Present");
        }
        else if (!presentAddr)
        {
            spdlog::warn("Present implementation address missing.");
            ok = false;
        }

        if (resizeBuffersAddr && !ResizeBuffersHookGlobal)
        {
            ResizeBuffersHookGlobal = safetyhook::create_inline(resizeBuffersAddr, reinterpret_cast<void*>(HookedResizeBuffers));
            LOG_HOOK(ResizeBuffersHookGlobal, "Global IDXGISwapChain::ResizeBuffers");
        }
        else if (!resizeBuffersAddr)
        {
            spdlog::warn("ResizeBuffers implementation address missing.");
            ok = false;
        }

        // Cleanup dummy
        sc.Reset();
        ctx.Reset();
        dev.Reset();
        DestroyWindow(hwnd);

        return ok;
    }
}

void D3D11Hooks::Initialize()
{
    if (InstallGlobalSwapchainHooksViaDummy())
    {
        spdlog::info("Global DXGI Present/Resize hooks installed via dummy swapchain.");
    }
    else
    {
        spdlog::warn("Global DXGI Present/Resize hooks not installed (dummy path failed).");
    }
}

void D3D11Hooks::UnloadCompiler(const HMODULE d3dcompiler)
{
    if (!g_VectorScalingFix.bNeedsCompiler)
    {
        FreeLibrary(d3dcompiler);
        spdlog::info("D3D11Hooks: Released d3dcompiler_43.dll as it is no longer needed.");
    }
}

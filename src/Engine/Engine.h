#pragma once

#include "Core/Win32.h"
#include "Input/InputState.h"
#include "Math/Camera.h"
#include "Platform/Windows/WindowsWindow.h"
#include "Renderer/SimpleSceneRenderer.h"
#include "RHI/D3D12RHI.h"
#include "Scene/SceneTypes.h"

#include <vector>
#include <string>

class FEngine
{
public:
    void Run(HINSTANCE hInstance);

private:
    static LRESULT WindowMessageHandler(void* userPtr, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleWindowMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    void Tick(float dtSeconds);
    void LayoutUI();
    void UpdateSkyUI();

private:
    enum class EGizmoAxis : uint8
    {
        None,
        X,
        Y,
        Z,
    };
    enum class EGizmoMode : uint8
    {
        Translate,
        Scale,
    };

    FWindowsWindow Window;
    FD3D12RHI RHI;
    FSimpleSceneRenderer Renderer;

    FInputState Input;
    FCamera Camera;

    // Sidebar UI (Win32 controls)
    HWND EngineNameLabel = nullptr;
    HWND SidebarList = nullptr;
    static constexpr int SidebarWidthPx = 200;
    WNDPROC SidebarOldProc = nullptr;
    bool SidebarMaybeDrag = false;
    int SidebarDownX = 0;
    int SidebarDownY = 0;
    uint64 SidebarDownTickMs = 0;

    // Sidebar lighting/atmosphere controls
    HWND RenderPathLabel = nullptr;
    HWND RenderPathCombo = nullptr;
    HWND LumenCheckbox = nullptr;
    HWND LumenSWRTCheckbox = nullptr;
    HWND LumenHWRTCheckbox = nullptr;
    HWND SkyEnableCheckbox = nullptr;
    HWND SunYawLabel = nullptr;
    HWND SunYawSlider = nullptr;
    HWND SunPitchLabel = nullptr;
    HWND SunPitchSlider = nullptr;
    HWND SunIntensityLabel = nullptr;
    HWND SunIntensitySlider = nullptr;
    HWND RayleighLabel = nullptr;
    HWND RayleighSlider = nullptr;
    HWND MieLabel = nullptr;
    HWND MieSlider = nullptr;
    HWND MieGLabel = nullptr;
    HWND MieGSlider = nullptr;
    HWND AtmoHeightLabel = nullptr;
    HWND AtmoHeightSlider = nullptr;
    HWND SkyLabel = nullptr;

    // Bottom assets/materials panel
    HWND BottomPanel = nullptr;
    static constexpr int BottomPanelHeightPx = 180;
    HWND TextureList = nullptr;
    HWND MaterialList = nullptr;
    HWND TexturePreview = nullptr;
    HWND NewMaterialBtn = nullptr;
    HWND TonemapCheckbox = nullptr;
    WNDPROC MaterialOldProc = nullptr;
    bool MaterialMaybeDrag = false;
    int MaterialDownX = 0;
    int MaterialDownY = 0;
    uint64 MaterialDownTickMs = 0;
    bool bDraggingMaterial = false;
    int DragMaterialIndex = -1;
    bool bPendingMaterialDrop = false;
    int PendingMaterialIndex = -1;
    int PendingDropMouseX = 0;
    int PendingDropMouseY = 0;
    int SelectedTextureIndex = -1;

    struct FTextureAsset
    {
        std::wstring Path;
        HBITMAP Preview = nullptr;
        DirectX::XMFLOAT3 AvgColor{ 1.0f, 1.0f, 1.0f };
        int RendererSlot = 0;
    };
    struct FMaterialAsset
    {
        std::wstring Name;
        DirectX::XMFLOAT3 Albedo{ 0.8f, 0.8f, 0.8f };
        float Metallic = 0.0f;
        float Roughness = 0.5f;
        int AlbedoTexIndex = -1;
        int NormalTexIndex = -1;
        int RoughnessTexIndex = -1;
        int MetallicTexIndex = -1;
        int AOTexIndex = -1;

        int AlbedoTexSlot = 0;
        int NormalTexSlot = 0;
        int RoughnessTexSlot = 0;
        int MetallicTexSlot = 0;
        int AOTexSlot = 0;

        int SRVBase = 0; // renderer descriptor table base
    };
    std::vector<FTextureAsset> Textures;
    std::vector<FMaterialAsset> Materials;

    // Bottom panel subclass (for file drop)
    WNDPROC BottomOldProc = nullptr;
    static LRESULT CALLBACK BottomWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    // Material editor window
    HWND MaterialEditorHwnd = nullptr;
    int EditingMaterialIndex = -1;
    static LRESULT CALLBACK MaterialEditorWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void OpenMaterialEditor(int materialIndex);
    void UpdateMaterialEditorControls();
    void ApplyMaterialEditorChanges();

    // Helpers
    void AddTextureFromFile(const std::wstring& path);

    // Viewport (child window hosting swapchain)
    HWND ViewportHwnd = nullptr;
    static LRESULT CALLBACK ViewportWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleViewportMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK SidebarWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK MaterialWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    // Scene
    std::vector<FSceneObject> Objects;
    int SelectedIndex = -1;
    FSceneObject::EType PaletteType = FSceneObject::EType::Sphere;
    bool bPlacingFromSidebar = false;
    DirectX::XMFLOAT3 PreviewPos{};
    bool bCommitPlacement = false;
    FSceneObject::EType CommitType = FSceneObject::EType::Sphere;

    float SunYaw = 0.0f;
    float SunPitch = -0.6f;
    float SunIntensity = 3.0f;
    bool bEnableTonemap = true;
    FSimpleSceneRenderer::ERenderPath RenderPath = FSimpleSceneRenderer::ERenderPath::Forward;
    bool bEnableLumen = true;
    bool bEnableLumenSWRT = false;
    bool bEnableLumenHWRT = true;

    FSkyAtmosphereSettings SkySettings{};
    EGizmoAxis ActiveAxis = EGizmoAxis::None;
    EGizmoMode GizmoMode = EGizmoMode::Translate;
    bool bDragging = false;
    float DragAxisS0 = 0.0f;
    DirectX::XMFLOAT3 DragStartPos{};
    DirectX::XMFLOAT3 DragStartScale{ 1.0f, 1.0f, 1.0f };
    int MouseX = 0;
    int MouseY = 0;

    bool bRunning = true;
    uint64 StartTickMs = 0;
    uint64 PrevTickMs = 0;

    // UE-like RMB look (hide cursor + lock to center)
    bool bRmbCursorHidden = false;
    POINT RmbSavedCursorPos{};

    // Viewport navigation tuning
    float CameraMoveSpeed = 3.0f;         // units/sec
    float CameraLookSensitivity = 0.0018f / 5.0f; // radians per raw mouse count
};

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
    /**
     * @brief 引擎主入口，创建窗口、初始化渲染器并进入主循环。
     * @param hInstance 应用实例句柄。
     * @return 无返回值；内部处理消息循环与渲染。
     * @note 阶段：应用启动与主循环阶段。
     */
    void Run(HINSTANCE hInstance);

private:
    /**
     * @brief Win32 消息入口的静态转发器。
     * @param userPtr 用户指针（通常为 FEngine 实例）。
     * @param hwnd 窗口句柄。
     * @param msg 消息类型。
     * @param wParam 消息参数。
     * @param lParam 消息参数。
     * @return LRESULT 消息处理结果。
     * @note 阶段：运行时消息分发阶段。
     */
    static LRESULT WindowMessageHandler(void* userPtr, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    /**
     * @brief 处理主窗口的 Win32 消息。
     * @param hwnd 窗口句柄。
     * @param msg 消息类型。
     * @param wParam 消息参数。
     * @param lParam 消息参数。
     * @return LRESULT 消息处理结果。
     * @note 阶段：运行时 UI/输入处理阶段。
     */
    LRESULT HandleWindowMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    /**
     * @brief 单帧更新逻辑（输入、交互、动画、场景更新）。
     * @param dtSeconds 帧间隔时间（秒）。
     * @return 无返回值。
     * @note 阶段：每帧更新阶段（渲染前）。
     */
    void Tick(float dtSeconds);
    /**
     * @brief 重新布局 UI（侧栏与底栏）。
     * @param 无。
     * @return 无返回值。
     * @note 阶段：窗口尺寸变化/初始化阶段。
     */
    void LayoutUI();
    /**
     * @brief 刷新天空参数在 UI 上的显示。
     * @param 无。
     * @return 无返回值。
     * @note 阶段：UI 同步阶段。
     */
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
    /**
     * @brief 底部面板窗口过程（处理拖拽文件等）。
     * @param hwnd 面板窗口句柄。
     * @param msg 消息类型。
     * @param wParam 消息参数。
     * @param lParam 消息参数。
     * @return LRESULT 消息处理结果。
     * @note 阶段：运行时 UI 交互阶段。
     */
    static LRESULT CALLBACK BottomWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    // Material editor window
    HWND MaterialEditorHwnd = nullptr;
    int EditingMaterialIndex = -1;
    /**
     * @brief 材质编辑器窗口过程。
     * @param hwnd 材质编辑器窗口句柄。
     * @param msg 消息类型。
     * @param wParam 消息参数。
     * @param lParam 消息参数。
     * @return LRESULT 消息处理结果。
     * @note 阶段：运行时 UI 交互阶段。
     */
    static LRESULT CALLBACK MaterialEditorWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    /**
     * @brief 打开材质编辑器并绑定指定材质。
     * @param materialIndex 材质索引。
     * @return 无返回值。
     * @note 阶段：UI 编辑器交互阶段。
     */
    void OpenMaterialEditor(int materialIndex);
    /**
     * @brief 将材质数据同步到编辑器控件。
     * @param 无。
     * @return 无返回值。
     * @note 阶段：UI 同步阶段。
     */
    void UpdateMaterialEditorControls();
    /**
     * @brief 将编辑器控件的值写回材质数据。
     * @param 无。
     * @return 无返回值。
     * @note 阶段：UI 编辑提交阶段。
     */
    void ApplyMaterialEditorChanges();

    // Helpers
    /**
     * @brief 从磁盘加载纹理并加入资源列表。
     * @param path 纹理文件路径。
     * @return 无返回值。
     * @note 阶段：资源导入阶段（编辑器交互）。
     */
    void AddTextureFromFile(const std::wstring& path);

    // Viewport (child window hosting swapchain)
    HWND ViewportHwnd = nullptr;
    /**
     * @brief 视口窗口过程（交换链宿主子窗口）。
     * @param hwnd 视口窗口句柄。
     * @param msg 消息类型。
     * @param wParam 消息参数。
     * @param lParam 消息参数。
     * @return LRESULT 消息处理结果。
     * @note 阶段：运行时视口输入处理阶段。
     */
    static LRESULT CALLBACK ViewportWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    /**
     * @brief 处理视口窗口的消息。
     * @param hwnd 视口窗口句柄。
     * @param msg 消息类型。
     * @param wParam 消息参数。
     * @param lParam 消息参数。
     * @return LRESULT 消息处理结果。
     * @note 阶段：运行时视口交互阶段。
     */
    LRESULT HandleViewportMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    /**
     * @brief 侧边栏窗口过程（拖拽放置对象）。
     * @param hwnd 侧边栏控件句柄。
     * @param msg 消息类型。
     * @param wParam 消息参数。
     * @param lParam 消息参数。
     * @return LRESULT 消息处理结果。
     * @note 阶段：编辑器交互阶段。
     */
    static LRESULT CALLBACK SidebarWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    /**
     * @brief 材质列表窗口过程（拖拽材质到场景）。
     * @param hwnd 材质列表控件句柄。
     * @param msg 消息类型。
     * @param wParam 消息参数。
     * @param lParam 消息参数。
     * @return LRESULT 消息处理结果。
     * @note 阶段：编辑器交互阶段。
     */
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

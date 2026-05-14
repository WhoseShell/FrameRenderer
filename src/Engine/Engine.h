#pragma once

#include "Core/Win32.h"
#include "Editor/EditorAssets.h"
#include "Input/InputState.h"
#include "Math/Camera.h"
#include "Platform/Windows/WindowsWindow.h"
#include "Renderer/SimpleSceneRenderer.h"
#include "RHI/D3D12RHI.h"
#include "Scene/SceneTypes.h"

#include <vector>
#include <string>
#include <filesystem>
#include <unordered_map>

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
    void SyncViewportBackbufferSize();
    void InitImGuiEditor();
    void ShutdownImGuiEditor();
    void BeginImGuiEditorFrame();
    void DrawImGuiEditor();
    void DrawImGuiMainMenu();
    void DrawImGuiPlaceActors();
    void DrawImGuiViewport();
    void DrawImGuiOutliner();
    void DrawImGuiDetails();
    void DrawImGuiContentDrawer();
    void DrawImGuiRenderSettings();
    void DrawImGuiMaterialEditor();
    void DrawImGuiGizmo();
    void RenderImGuiDrawData(ID3D12GraphicsCommandList* cmd);
    void HideLegacyWin32EditorControls();
    static void ImGuiAllocateSrv(struct ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* outCpu, D3D12_GPU_DESCRIPTOR_HANDLE* outGpu);
    static void ImGuiFreeSrv(struct ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE gpu);
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

    HFONT UIFont = nullptr;
    HFONT UITitleFont = nullptr;
    HBRUSH UIBackgroundBrush = nullptr;
    HBRUSH UIPanelBrush = nullptr;
    HBRUSH UIHeaderBrush = nullptr;
    HBRUSH UIListBrush = nullptr;
    HBRUSH UIEditBrush = nullptr;

    // Sidebar UI (Win32 controls)
    HWND ToolbarPanel = nullptr;
    HWND ToolbarTitle = nullptr;
    HWND ToolbarNewLevelBtn = nullptr;
    HWND ToolbarOpenLevelBtn = nullptr;
    HWND ToolbarSaveLevelBtn = nullptr;
    HWND ToolbarImportObjBtn = nullptr;
    HWND ToolbarPlaceBtn = nullptr;
    HWND ToolbarSettingsBtn = nullptr;
    WNDPROC ToolbarSettingsOldProc = nullptr;
    HWND StatusLabel = nullptr;
    static constexpr int TopToolbarHeightPx = 40;

    HWND EngineNameLabel = nullptr;
    HWND SidebarToggleBtn = nullptr;
    HWND SidebarSearchEdit = nullptr;
    HWND SidebarList = nullptr;
    HWND SidebarBasicLabel = nullptr;
    HWND SidebarRenderDocLabel = nullptr;
    static constexpr int SidebarWidthPx = 280;
    bool bPlaceActorsOpen = true;
    WNDPROC SidebarSearchOldProc = nullptr;
    WNDPROC SidebarOldProc = nullptr;
    bool SidebarMaybeDrag = false;
    int SidebarDownX = 0;
    int SidebarDownY = 0;
    uint64 SidebarDownTickMs = 0;

    // Sidebar lighting/atmosphere controls
    HWND RenderPathLabel = nullptr;
    HWND RenderSettingsLabel = nullptr;
    HWND RenderGILabel = nullptr;
    HWND SunSectionLabel = nullptr;
    HWND AtmosphereSectionLabel = nullptr;
    HWND RenderPathCombo = nullptr;
    HWND LumenCheckbox = nullptr;
    HWND LumenSWRTCheckbox = nullptr;
    HWND LumenHWRTCheckbox = nullptr;
    HWND SkyEnableCheckbox = nullptr;
    HWND SunYawLabel = nullptr;
    HWND SunYawValueLabel = nullptr;
    HWND SunYawSlider = nullptr;
    HWND SunPitchLabel = nullptr;
    HWND SunPitchValueLabel = nullptr;
    HWND SunPitchSlider = nullptr;
    HWND SunIntensityLabel = nullptr;
    HWND SunIntensityValueLabel = nullptr;
    HWND SunIntensitySlider = nullptr;
    HWND RayleighLabel = nullptr;
    HWND RayleighValueLabel = nullptr;
    HWND RayleighSlider = nullptr;
    HWND MieLabel = nullptr;
    HWND MieValueLabel = nullptr;
    HWND MieSlider = nullptr;
    HWND MieGLabel = nullptr;
    HWND MieGValueLabel = nullptr;
    HWND MieGSlider = nullptr;
    HWND AtmoHeightLabel = nullptr;
    HWND AtmoHeightValueLabel = nullptr;
    HWND AtmoHeightSlider = nullptr;
    HWND SkyLabel = nullptr;

    // Bottom assets/materials panel
    HWND BottomPanel = nullptr;
    static constexpr int BottomPanelHeightPx = 260;
    HWND ContentTitleLabel = nullptr;
    HWND ContentDrawerToggleBtn = nullptr;
    HWND TextureTitleLabel = nullptr;
    HWND PreviewTitleLabel = nullptr;
    HWND MaterialTitleLabel = nullptr;
    HWND ContentPathLabel = nullptr;
    HWND ContentActionsLabel = nullptr;
    HWND ContentFoldersList = nullptr;
    HWND ContentHintLabel = nullptr;
    HWND ContentList = nullptr;
    HWND ImportObjBtn = nullptr;
    HWND PlaceAssetBtn = nullptr;
    HWND NewLevelBtn = nullptr;
    HWND OpenLevelBtn = nullptr;
    HWND SaveLevelBtn = nullptr;
    HWND TextureList = nullptr;
    HWND MaterialList = nullptr;
    HWND TexturePreview = nullptr;
    HWND NewMaterialBtn = nullptr;
    HWND TonemapCheckbox = nullptr;
    enum class EContentFilter : uint8
    {
        Models,
        Textures,
        Materials,
        Levels,
    };
    EContentFilter ContentFilter = EContentFilter::Models;
    bool bContentDrawerOpen = true;
    std::vector<int> ContentListAssetIndices;
    std::vector<FSceneObject::EType> PaletteListTypes;
    std::wstring ActorPaletteFilter;
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
    bool bAssetPreviewActive = false;
    FSceneObject AssetPreviewObject{};
    std::wstring AssetPreviewLabel;
    int AssetPreviewMaterialIndex = -1;
    HBITMAP GeneratedPreviewBitmap = nullptr;

    struct FTextureAsset
    {
        std::wstring Name;
        std::wstring RelativePath;
        std::wstring Path;
        HBITMAP Preview = nullptr;
        DirectX::XMFLOAT3 AvgColor{ 1.0f, 1.0f, 1.0f };
        int RendererSlot = 0;
    };
    struct FMaterialAsset
    {
        std::wstring Name;
        std::wstring AssetPath;
        EMaterialShadingMode ShadingMode = EMaterialShadingMode::PbrLit;
        DirectX::XMFLOAT3 Albedo{ 0.8f, 0.8f, 0.8f };
        float Metallic = 0.0f;
        float Roughness = 0.5f;
        float UnlitIntensity = 1.0f;
        float RockNormalStrength = 0.18f;
        float RockBaseColorBoost = 1.25f;
        int AlbedoTexIndex = -1;
        int NormalTexIndex = -1;
        int RoughnessTexIndex = -1;
        int MetallicTexIndex = -1;
        int AOTexIndex = -1;
        std::wstring AlbedoTexPath;
        std::wstring NormalTexPath;
        std::wstring RoughnessTexPath;
        std::wstring MetallicTexPath;
        std::wstring AOTexPath;

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
    HWND RenderSettingsHwnd = nullptr;
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
    static LRESULT CALLBACK RenderSettingsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK ToolbarSettingsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
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
    void OpenRenderSettingsDialog();
    void LayoutRenderSettingsDialog();
    void SyncRenderSettingsControls();

    // Helpers
    /**
     * @brief 从磁盘加载纹理并加入资源列表。
     * @param path 纹理文件路径。
     * @return 无返回值。
     * @note 阶段：资源导入阶段（编辑器交互）。
     */
    void AddTextureFromFile(const std::wstring& path);
    void InitializeEditorContent();
    void RefreshActorPalette();
    void RefreshContentBrowser();
    void RefreshOutliner();
    void RefreshDetailsPanel();
    void UpdateStatusText();
    void ApplyEditorFont(HWND hwnd, bool title = false);
    LRESULT ApplyEditorControlColors(HWND control, HDC hdc, UINT msg);
    HWND CreateEditorLabel(const wchar_t* text, HWND parent, bool title = false);
    HWND CreateEditorButton(const wchar_t* text, HWND parent, int id);
    HWND CreateEditorCheckbox(const wchar_t* text, HWND parent, int id);
    HWND CreateEditorList(HWND parent, int id);
    void SelectContentFilter(EContentFilter filter);
    bool DoesAssetPassContentFilter(const editor::FAssetRecord& asset) const;
    std::wstring ContentFilterDisplayName(EContentFilter filter) const;
    void SetSelectedIndex(int index);
    void MarkLevelDirty();
    void UpdateWindowTitle(const wchar_t* baseTitle, const wchar_t* pathName);
    void NewLevel();
    void SaveCurrentLevel(bool saveAs);
    void OpenLevelFromDialog();
    void LoadLevelFromPath(const std::filesystem::path& path);
    void ImportObjFromDialog();
    void PlaceSelectedContentAsset();
    void PreviewSelectedContentAsset();
    void PreviewContentAsset(const editor::FAssetRecord& asset);
    void PreviewTextureAsset(const editor::FAssetRecord& asset);
    void PreviewMaterialAsset(int materialIndex, bool openEditor);
    void PreviewModelAsset(const editor::FAssetRecord& asset);
    void ClearAssetPreview();
    void FocusViewportOnObject(int objectIndex);
    void SetPreviewBitmap(HBITMAP bitmap, bool takeOwnership);
    void SelectTextureIndex(int textureIndex);
    HBITMAP CreateMaterialPreviewBitmap(const DirectX::XMFLOAT3& color) const;
    int EnsureStaticMeshLoaded(const std::wstring& relativePath);
    int EnsureTextureLoaded(const std::wstring& relativePath);
    int FindTextureByRelativePath(const std::wstring& relativePath) const;
    int FindMaterialByAssetPath(const std::wstring& relativePath) const;
    void LoadContentMaterials();
    void MigrateContentMaterialsToV2();
    void SaveMaterialAsset(int materialIndex);
    void ApplyMaterialToObject(int objectIndex, int materialIndex);
    bool EnsureDefaultMaterialAsset();
    int EnsureDefaultMaterialLoaded();
    bool EnsureConcreteMaterialForObject(FSceneObject& object);
    bool EnsureConcreteMaterialsForRenderableObjects();
    void UpdateMaterialRuntimeBindings(FMaterialAsset& mat);
    void ApplyMaterialAssetToSceneObject(FSceneObject& object, int materialIndex) const;
    FSceneObject MakeSceneObject(FSceneObject::EType type, const DirectX::XMFLOAT3& position, const std::wstring& assetPath = L"");
    editor::FLevelFile BuildLevelFile() const;
    void ApplyLevelFile(const editor::FLevelFile& level, const std::filesystem::path& path);
    static bool IsMaterialAssignableObject(const FSceneObject& object);
    static std::wstring SceneObjectTypeToString(FSceneObject::EType type);
    static FSceneObject::EType SceneObjectTypeFromString(const std::wstring& type);
    void ApplyDetailsEdits();
    void EnsureDefaultEnvironmentActors();
    const FSceneObject* FindActiveSunLight() const;
    FSceneObject* FindActiveSunLight();
    const FSceneObject* FindActiveSkyAtmosphere() const;
    DirectX::XMFLOAT3 GetActiveLightDirection() const;
    float GetActiveSunIntensity() const;
    FSkyAtmosphereSettings GetActiveSkySettings() const;
    static DirectX::XMFLOAT3 DirectionToSunPosition(float yaw, float pitch, float distance);
    static void SunPositionToAngles(const DirectX::XMFLOAT3& position, float& yaw, float& pitch);

    // Viewport (child window hosting swapchain)
    HWND ViewportHwnd = nullptr;
    uint32 PendingViewportWidth = 0;
    uint32 PendingViewportHeight = 0;
    uint64 PendingViewportResizeTickMs = 0;
    uint64 LastViewportResizeTickMs = 0;
    static constexpr uint64 ViewportResizeDebounceMs = 250;
    static constexpr uint64 ViewportResizeRetryMs = 500;
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
    static LRESULT CALLBACK SidebarSearchWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
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

    // UE-like editor state
    static constexpr int RightPanelWidthPx = 300;
    HWND RightPanel = nullptr;
    HWND OutlinerLabel = nullptr;
    HWND OutlinerList = nullptr;
    HWND DetailsLabel = nullptr;
    HWND DetailTransformLabel = nullptr;
    HWND DetailEnvironmentLabel = nullptr;
    HWND DetailNameLabel = nullptr;
    HWND DetailPositionLabel = nullptr;
    HWND DetailScaleLabel = nullptr;
    HWND DetailNameEdit = nullptr;
    HWND DetailPosXEdit = nullptr;
    HWND DetailPosYEdit = nullptr;
    HWND DetailPosZEdit = nullptr;
    HWND DetailScaleXEdit = nullptr;
    HWND DetailScaleYEdit = nullptr;
    HWND DetailScaleZEdit = nullptr;
    HWND DetailIntensityLabel = nullptr;
    HWND DetailIntensityEdit = nullptr;
    HWND DetailSkyEnabledCheckbox = nullptr;
    HWND DetailRayleighLabel = nullptr;
    HWND DetailRayleighEdit = nullptr;
    HWND DetailMieLabel = nullptr;
    HWND DetailMieEdit = nullptr;
    HWND DetailMieGLabel = nullptr;
    HWND DetailMieGEdit = nullptr;
    HWND DetailAtmosphereHeightLabel = nullptr;
    HWND DetailAtmosphereHeightEdit = nullptr;
    HWND ApplyDetailsBtn = nullptr;
    bool bSuppressOutlinerEvents = false;

    bool bUseImGuiEditor = true;
    bool bImGuiInitialized = false;
    bool bShowImGuiSettings = false;
    bool bShowImGuiMaterialEditor = false;
    bool bImGuiViewportHovered = false;
    bool bImGuiViewportFocused = false;
    int ImGuiViewportScreenX = 0;
    int ImGuiViewportScreenY = 0;
    int ImGuiViewportX = 0;
    int ImGuiViewportY = 0;
    int ImGuiViewportW = 1;
    int ImGuiViewportH = 1;
    int ImGuiSelectedContentListIndex = -1;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> ImGuiSrvHeap;
    uint32 ImGuiSrvDescriptorSize = 0;
    uint32 ImGuiNextSrvDescriptor = 0;
    uint32 ImGuiSrvDescriptorCapacity = 64;

    std::filesystem::path ContentRoot;
    editor::FContentLayout ContentLayout{};
    std::vector<editor::FAssetRecord> ContentAssets;
    std::unordered_map<std::wstring, int> StaticMeshByAsset;
    std::unordered_map<std::wstring, float> StaticMeshRadiusByAsset;
    uint32 NextObjectId = 1;
    std::filesystem::path CurrentLevelPath;
    std::wstring CurrentLevelName = L"Untitled";
    bool bLevelDirty = false;
};

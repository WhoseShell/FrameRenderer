#pragma once

#include <functional>
#include <vector>

#include <d3d12.h>

class FRenderGraphBuilder
{
public:
    /**
     * @brief 构建一个简单的渲染图执行器。
     * @param cmdList 目标命令列表。
     * @return 无返回值（构造函数）。
     * @note 阶段：渲染帧构建阶段。
     */
    explicit FRenderGraphBuilder(ID3D12GraphicsCommandList* cmdList)
        : CmdList(cmdList)
    {
    }

    template <typename TFunc>
    /**
     * @brief 添加一个渲染 Pass（延迟执行）。
     * @param name Pass 名称（调试用）。
     * @param func Pass 执行函数，接受命令列表。
     * @return 无返回值。
     * @note 阶段：渲染图构建阶段。
     */
    void AddPass(const char* name, TFunc&& func)
    {
        // 将可调用对象封装为统一签名，便于顺序执行。
        Passes.push_back({ name, std::function<void(ID3D12GraphicsCommandList*)>(std::forward<TFunc>(func)) });
    }

    /**
     * @brief 按添加顺序执行所有 Pass。
     * @param 无。
     * @return 无返回值。
     * @note 阶段：渲染帧执行阶段。
     */
    void Execute()
    {
        // 顺序调用各 Pass 的回调。
        for (auto& pass : Passes)
            pass.Execute(CmdList);
    }

private:
    struct FPass
    {
        const char* Name = nullptr;
        std::function<void(ID3D12GraphicsCommandList*)> Execute;
    };

    ID3D12GraphicsCommandList* CmdList = nullptr;
    std::vector<FPass> Passes;
};


#pragma once

#include <functional>
#include <vector>

#include <d3d12.h>

class FRenderGraphBuilder
{
public:
    explicit FRenderGraphBuilder(ID3D12GraphicsCommandList* cmdList)
        : CmdList(cmdList)
    {
    }

    template <typename TFunc>
    void AddPass(const char* name, TFunc&& func)
    {
        Passes.push_back({ name, std::function<void(ID3D12GraphicsCommandList*)>(std::forward<TFunc>(func)) });
    }

    void Execute()
    {
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


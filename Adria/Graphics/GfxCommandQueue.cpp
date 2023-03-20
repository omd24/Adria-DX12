#include "GfxCommandQueue.h"
#include "GfxDevice.h"
#include "GfxCommandList.h"
#include "../Utilities/StringUtil.h"

namespace adria
{
	bool GfxCommandQueue::Create(GfxDevice* gfx, GfxCommandListType type, char const* name /*= ""*/)
	{
		ID3D12Device* device = gfx->GetDevice();
		D3D12_COMMAND_QUEUE_DESC queue_desc{};
		queue_desc.Type = GetCommandListType(type);
		queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
		queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
		queue_desc.NodeMask = 0;
		HRESULT hr = device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(command_queue.GetAddressOf()));
		if (FAILED(hr)) return false;
		command_queue->SetName(ToWideString(name).c_str());
		if(type != GfxCommandListType::Copy) command_queue->GetTimestampFrequency(&timestamp_frequency);
		return true;
	}

	void GfxCommandQueue::ExecuteCommandLists(std::span<ID3D12CommandList*> cmd_lists)
	{
		command_queue->ExecuteCommandLists((uint32)cmd_lists.size(), cmd_lists.data());
	}

	void GfxCommandQueue::ExecuteCommandLists(std::span<GfxCommandList*> cmd_lists)
	{
		std::vector<ID3D12CommandList*> d3d12_cmd_lists(cmd_lists.size());
		for (size_t i = 0; i < d3d12_cmd_lists.size(); ++i) d3d12_cmd_lists[i] = cmd_lists[i]->GetNative();
		command_queue->ExecuteCommandLists((uint32)d3d12_cmd_lists.size(), d3d12_cmd_lists.data());
	}

	void GfxCommandQueue::Signal(GfxFence& fence, uint64 fence_value)
	{
		command_queue->Signal(fence, fence_value);
	}

	void GfxCommandQueue::Wait(GfxFence& fence, uint64 fence_value)
	{
		command_queue->Wait(fence, fence_value);
	}

}

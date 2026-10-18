#pragma once
#ifndef d3dimgui_dxgi_hook_h_
#define d3dimgui_dxgi_hook_h_

#include <wrl/client.h>

struct ID3D12CommandQueue;

namespace d3d_imgui_layer::hook
{
struct options
{
    bool swapchain = true;   // present resizebuffers
    bool present1 = true;    // idxgiswapchain1 的 present1
    bool command_queue = true; // executecommandlists 蹲点
    bool cursor = true;      // user32 四个光标函数
};

bool install(const options& opt);
void shutdown();

// minhook 的状态是进程级一份 dxgi 和 opengl 两条路都从这里要
// 只开 opengl 那条时没人先初始化它 钩子就会一直装不上 拆仍由 shutdown 统一做
bool ensure_minhook();

// 观察到的 direct 队列 或者用户手动塞进来的 都没有就返回 false
bool get_command_queue(Microsoft::WRL::ComPtr<ID3D12CommandQueue>& out);
void set_command_queue(ID3D12CommandQueue* queue);

// 进程里可能不止一条 direct 队列 拿别人设备的队列提交就是跨设备误用 运行时会把设备判死
// device 传 swapchain 自己的设备 匹配不到时退回第一条观察到的
bool match_command_queue(void* device, Microsoft::WRL::ComPtr<ID3D12CommandQueue>& out);
// 绑成功之后关掉蹲点 executecommandlists 每帧都要过钩子 少一次虚调用少一分干扰
void lock_command_queue();
// 设备级失败之后队列八成已经换了 抓住不放就是把僵尸对象带进宿主的收尾
void forget_command_queues();
}

#endif

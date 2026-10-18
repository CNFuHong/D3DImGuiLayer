// slot_check.c 把钩子要用的 vtable 槽位号在编译期钉死
//
// 槽位号不是特征 它是 com 的 abi 微软不能动 动了所有现存的二进制都崩
// 所以真正需要防的不是游戏环境 是我们自己数错 以及哪天头文件变了
// 这份文件只用 sdk 头里的 c vtbl 结构算偏移 每次编译都重新核一遍
// 数错或者布局变了 这里直接编译不过 不会等到运行时在别人的进程里炸

#include <dxgi.h>
#include <dxgi1_2.h>
#include <dxgi1_4.h>
#include <d3d12.h>
#include <stddef.h>

#define SLOT(table, member) (offsetof(table, member) / sizeof(void*))

/* 负数长度的数组就是编译错误 比 static_assert 更挑编译器 */
typedef char slot_present_is_8[
    SLOT(IDXGISwapChainVtbl, Present) == 8u ? 1 : -1];
typedef char slot_get_buffer_is_9[
    SLOT(IDXGISwapChainVtbl, GetBuffer) == 9u ? 1 : -1];
typedef char slot_get_desc_is_12[
    SLOT(IDXGISwapChainVtbl, GetDesc) == 12u ? 1 : -1];
typedef char slot_resize_buffers_is_13[
    SLOT(IDXGISwapChainVtbl, ResizeBuffers) == 13u ? 1 : -1];
typedef char slot_present1_is_22[
    SLOT(IDXGISwapChain1Vtbl, Present1) == 22u ? 1 : -1];
typedef char slot_get_current_back_buffer_is_36[
    SLOT(IDXGISwapChain3Vtbl, GetCurrentBackBufferIndex) == 36u ? 1 : -1];
typedef char slot_execute_command_lists_is_10[
    SLOT(ID3D12CommandQueueVtbl, ExecuteCommandLists) == 10u ? 1 : -1];
typedef char slot_queue_get_desc_is_18[
    SLOT(ID3D12CommandQueueVtbl, GetDesc) == 18u ? 1 : -1];

/* 这些只是为了让上面几个 typedef 不被优化掉当成无用的东西 编译期检查已经生效 */
typedef char slot_check_compiled[1];

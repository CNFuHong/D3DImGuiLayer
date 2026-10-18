#include <Windows.h>

#include <d3d_imgui_layer/d3dimgui.h>

#include "core/layer.h"

namespace
{
// 不能在 DllMain 里装钩子 那时还握着加载器锁 d3d 相关 dll 可能没就位
DWORD WINAPI startup_thread(LPVOID parameter)
{
    HMODULE module = static_cast<HMODULE>(parameter);

    if (d3dimgui_startup(nullptr) == 0)
    {
#if d3dimgui_self_unload
        // 起不来就把自己的映射拆掉 不给宿主进程留没用的模块
        FreeLibraryAndExitThread(module, 0);
#else
        // 起不来也已经拆干净了 镜像留在这儿
        // 刚才可能有线程正走进我们那些 detour 现在把页抽走就是送它一个野地址
        return 0;
#endif
    }

    // 起来了就一直待到进程结束 带钩子的 dll 不适合半路卸载
    return 0;
}
}

BOOL APIENTRY DllMain(HMODULE module, const DWORD reason, LPVOID reserved)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
    {
        DisableThreadLibraryCalls(module);

        HANDLE thread = CreateThread(nullptr, 0, startup_thread, module, 0, nullptr);
        if (thread == nullptr)
            return FALSE;

        CloseHandle(thread);
        break;
    }
    case DLL_PROCESS_DETACH:
    {
        // 进程退出这条路 reserved 非空 此时加载器锁还握着 别的线程已经被拆掉了
        // 在这种状态里摘钩子就是等一个永远不会回来的线程 所以只标记不再干活
        d3d_imgui_layer::set_process_exiting();
        if (reserved == nullptr)
            d3dimgui_shutdown();
        break;
    }
    default:
        break;
    }

    return TRUE;
}

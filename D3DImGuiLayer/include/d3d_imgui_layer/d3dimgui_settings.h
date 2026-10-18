// d3dimgui_settings.h - 编译期配置 二次开发第一个要改的文件
// 这里的值全部是代码内常量 框架不读任何本地配置文件
#pragma once
#ifndef d3dimgui_settings_h_
#define d3dimgui_settings_h_

// 导出开关 由工程预定义 d3dimgui_export_symbols 决定
// 直接把源码编进自己的模块时两个宏都不定义 导出标记为空
#ifndef d3dimgui_export
#  if defined(d3dimgui_export_symbols)
#    define d3dimgui_export __declspec(dllexport)
#  elif defined(d3dimgui_import_symbols)
#    define d3dimgui_export __declspec(dllimport)
#  else
#    define d3dimgui_export
#  endif
#endif

// 框架内置断言 关闭后渲染路径里不保留任何检查
#ifndef d3dimgui_enable_assert
#define d3dimgui_enable_assert 1
#endif

// 挂钩点 关掉 install_hooks 就退化成纯手动渲染层
#ifndef d3dimgui_install_hooks
#define d3dimgui_install_hooks 1
#endif

// 额外钩 IDXGISwapChain1::Present1 vtable 22
// 开 hdr 或用 waitable object 的引擎只走 Present1 不钩就一帧都不出
#ifndef d3dimgui_hook_present1
#define d3dimgui_hook_present1 1
#endif

// 钩 opengl32 的 wglSwapBuffers 走 gl 的游戏里 dxgi 那条一次都不会命中
// 关掉它 gl 游戏就完全没界面 基岩版这类走 gl 的目标全靠这条
#ifndef d3dimgui_hook_wgl_swap
#define d3dimgui_hook_wgl_swap 1
#endif

// imgui 的 gl 后端要的 glsl 版本串 130 是 gl3.0 兼容范围最宽的一档
// 游戏要是只给到 gl2.1 就改 120 想要新特性再往上调
#ifndef d3dimgui_glsl_version
#define d3dimgui_glsl_version "#version 130"
#endif

// dx12 靠钩 ExecuteCommandLists 蹲点抓 direct 队列 关掉则只能手动喂队列
#ifndef d3dimgui_hook_command_queue
#define d3dimgui_hook_command_queue 1
#endif

// 钩 user32 的 SetCursor SetCursorPos ClipCursor 防止游戏抢回鼠标
#ifndef d3dimgui_hook_cursor
#define d3dimgui_hook_cursor 1
#endif

// 菜单打开时吞掉发往游戏窗口的键盘鼠标消息
#ifndef d3dimgui_suppress_input
#define d3dimgui_suppress_input 1
#endif

// 默认呼出键 vk_insert 运行期可用 d3dimgui_set_toggle_key 改
// vk_insert 是 0x2d 0x36 是数字键 6 按错了就是游戏里一放技能就切菜单
#ifndef d3dimgui_toggle_key
#define d3dimgui_toggle_key 0x2d // VK_INSERT
#endif

// 默认卸载键 vk_end 按下去摘钩拆后端 镜像放不放由 d3dimgui_self_unload 决定
// 设成 0 表示不绑这个键 只能自己调 d3dimgui_unload
#ifndef d3dimgui_unload_key
#define d3dimgui_unload_key 0x23 // VK_END
#endif

// 默认字号 调用方没塞字体时用 imgui 内置字体
#ifndef d3dimgui_default_font_pixel
#define d3dimgui_default_font_pixel 18.0f
#endif

// 是否默认显示自带分辨率缩放
#ifndef d3dimgui_apply_dpi_scale
#define d3dimgui_apply_dpi_scale 1
#endif

// dx12 的 srv 描述符槽数 一张字体图集加少量贴图够用
#ifndef d3dimgui_srv_slots
#define d3dimgui_srv_slots 64
#endif

// 采 vtable 用的虚拟窗口类名 改这里可以避免和别人撞
#ifndef d3dimgui_dummy_window_class
#define d3dimgui_dummy_window_class L"d3d_imgui_layer_dummy"
#endif

// 采 vtable 用的虚拟窗口边长 也是 dummy 交换链的尺寸 像素
#ifndef d3dimgui_dummy_size
#define d3dimgui_dummy_size 64
#endif

// 允许同时登记的 swapchain 数 只有一条会真正出图
#ifndef d3dimgui_max_targets
#define d3dimgui_max_targets 8
#endif

// 主目标判定权重 顶层窗口和面积优先 面积不足屏幕这个比例算小窗
#ifndef d3dimgui_primary_area_ratio
#define d3dimgui_primary_area_ratio 0.30f
#endif

// 采 vtable 时是否保留 warp 降级
#ifndef d3dimgui_allow_warp
#define d3dimgui_allow_warp 1
#endif

// 启动就开一个独立控制台打日志 注入进没有调试器的进程时用
// 运行期也可以随时用 d3dimgui_open_console 开关
#ifndef d3dimgui_start_with_console
#define d3dimgui_start_with_console 0
#endif

// 日志同时写 %TEMP%\d3dimgui_layer.log 控制台跟着进程一起没 崩溃恰好在那一刻
#ifndef d3dimgui_log_to_file
#define d3dimgui_log_to_file 1
#endif

// 装一个只记录不拦截的 vectored 异常观察者 把崩溃地址所属模块和偏移写进日志
#ifndef d3dimgui_crash_observer
#define d3dimgui_crash_observer 1
#endif

// 卸载时是否真的把镜像从宿主进程放掉
// detour 跑在宿主的调用栈上 还有线程站在我们的代码里就 unmap 是必崩的
// 默认只拆干净不 unmap 需要真卸载再打开
#ifndef d3dimgui_self_unload
#define d3dimgui_self_unload 0
#endif

// 注入后默认画一个小的信息窗 这是"挂上了"的可见证据
// 关掉它就什么都不画 只留用户自己写的东西
#ifndef d3dimgui_show_info
#define d3dimgui_show_info 1
#endif

// src/user/draw.cpp 里默认带一张程序生成的测试图和一组矢量图形
// 用来确认纹理真的上传成功 写完自己的东西就改 0
#ifndef d3dimgui_show_demo
#define d3dimgui_show_demo 1
#endif

// primary 认定多少帧没在 present 里出现就让位
// 游戏重建交换链或者关掉开场窗口之后 就靠这一步重新认目标
#ifndef d3dimgui_stale_primary_frames
#define d3dimgui_stale_primary_frames 120
#endif

// 渲染路径连续失败几次就永久停用渲染层 宁可没界面也别把宿主带崩
#ifndef d3dimgui_render_failure_fuse
#define d3dimgui_render_failure_fuse 3
#endif

// 连续重绑这么多次还没画出来 就是认目标或者建资源在原地打转 停用
#ifndef d3dimgui_rebind_fuse
#define d3dimgui_rebind_fuse 12
#endif

#endif

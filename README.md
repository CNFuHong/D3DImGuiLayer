# d3dimgui layer

注入进去就能出图的 imgui 渲染层 x64 支持 d3d11 d3d12 和 opengl

它管的事情很窄 找提交点 建 imgui 上下文 接管输入和光标 每帧把绘制数据交给宿主已经建好的设备
它不管 认目标偏移 找签名 读配置 全部配置在代码里

## 定位

这颗是自用框架 新项目要一个界面就从这里复制一份 改 `src/user/draw.cpp` 和那颗配置头 重编出 dll 注入进去
同一套源码也能直接编进你自己的模块 那种用法不需要任何导出标记
接口面不为外部使用者铺路 也不做插件热重载 带 inline hook 的模块半路放掉镜像是要崩宿主的

## 现在能做什么

- 三条呈现路径 d3d11 d3d12 opengl 全部 x64
- 提交点自己找 dxgi 走 `IDXGISwapChain::Present` 和 `IDXGISwapChain1::Present1` d3d12 另外蹲 `ExecuteCommandLists` 抓 direct 队列 opengl 走 `wglSwapBuffers`
- 关掉自带钩子能退成纯手动渲染层 你在真 present 之前调 `d3dimgui_draw` gl 那条调 `d3dimgui_draw_gl`
- 输入 子类化宿主窗口 窗口线程只排队列 喂给 imgui 的动作全在渲染线程做 免得在宿主的调用栈里重入
- 光标 钩 user32 的 SetCursor ShowCursor ClipCursor SetCursorPos 只在菜单态生效
- 恢复 设备级错误 resizebuffers 失败 窗口销毁 交换链重建 这几条都有用例守着
- 兜底 整条绘制路径包 SEH 连续渲染失败或者原地重绑超次数就自己停用 宁可没界面也不把宿主带崩
- 诊断 文件日志 只记录不拦截的崩溃观察者 环境自报 适配器 交换链描述 帧延迟 加一句"这块代码已经被别人 inline 改过"的共存提示

## 挂钩点是怎么来的

不用特征码 也不依赖任何游戏的偏移
启动时自己造一次 dummy device 和 dummy 交换链 从它们的虚表里读出 `Present` 这些函数的地址 再对这些地址下钩
虚表槽位是 COM 冻结的 abi 不随系统版本或驱动变 所以工程里放了一份 `src/hook/slot_check.c` 用 `offsetof` 把每个槽位在编译期核死 数错或者头文件换版会当场编译失败 而不是到宿主进程里炸
opengl 那条更省事 `wglSwapBuffers` 是导出函数 不采槽 而且 opengl32 不是静态导入 wgl 入口全在运行期取址 注进一个不用 gl 的进程不会替它多带一个渲染模块

一条铁律别回退 后端只借宿主的交换链指针 不多持一份引用 一个窗口同一时刻只能挂一条 flip 链 多攥一个引用 宿主重建交换链就是 `DXGI_ERROR_INVALID_CALL` 症状是黑屏或者崩 看着像引擎自己的锅

## 快速开始

前置 只有 x64 和 Windows SDK 要 三方库是原样拷进来的 没有包管理器 运行时库静态 `/MT`
`.slnx` 要比较新的 MSBuild 才认 手上版本报 `MSB1009 项目文件不存在` 就直接编 `D3DImGuiLayer\D3DImGuiLayer.vcxproj` 一样的

```
MSBuild D3DImGuiLayer.slnx -p:Configuration=Release -p:Platform=x64
```

产物 `x64\Release\D3DImGuiLayer.dll` 同目录有 pdb 和那颗导入库 导入库是给手动模式链接用的 obj 甩在 `x64\cache\` 这些都不进版本库

注入之后默认能看见两块东西

- `d3dimgui info` 一行行的现场信息 api 加后端名 state 和错误码 分辨率和 dpi 缓冲区索引 帧号和 fps 提交对象地址 顶层窗口和登记到的表面数 走的是哪条提交路径 显卡名 dx12 再带一行抓到的队列 这就是"挂上了"的证据 gl 那两行标签会换成 `hdc` 和 `context` 而且不显示缓冲区索引 别按交换链去读
- `d3dimgui demo` 程序生成的测试纹理加一组矢量图形 纹理没上传或者画错热区 一眼就能看出来

不要它们就把 `d3dimgui_show_info` 和 `d3dimgui_show_demo` 改 0 重编

键位 默认 `VK_INSERT` 0x2d 切菜单态 只有菜单态才吞宿主的键鼠 默认不吞 `VK_END` 0x23 卸载 拆钩子拆资源 镜像放不放由 `d3dimgui_self_unload` 决定 默认不放

## 写自己的界面

主落点是 `src/user/draw.cpp` 里面三个函数 由框架在固定时机调

- `initialize` 后端建好之后一次 字体 登记菜单页 一次性资源都放这儿
- `frame` 每帧在 `NewFrame` 之后 `Render` 之前 直接写 imgui 调用 常驻绘制走这条
- `shutdown` 拆后端之前 自己申请的资源在这儿放掉 尤其是纹理 上下文重建之后旧纹理指针就是野的

要一扇能开关的菜单就 `d3dimgui_add_menu_tab("名字", 回调, 用户数据)` 那种页只在菜单态画 得按 INSERT 才出来 常驻的东西别往这儿塞 这是最容易搞混的一处

中文不用配字符区间 imgui 1.92 是按需烘字形 `d3dimgui_load_font` 塞进去的 ttf 里有字形就能显示 内置那套字体只有 ascii 所以信息窗标签全是英文

## 配置在哪

`include/d3d_imgui_layer/d3dimgui_settings.h` 每个开关旁边都有注释 按用途分四组

- 挂钩 `d3dimgui_install_hooks` `d3dimgui_hook_present1` `d3dimgui_hook_command_queue` `d3dimgui_hook_wgl_swap` `d3dimgui_glsl_version` `d3dimgui_hook_cursor`
- 键鼠与外观 `d3dimgui_toggle_key` `d3dimgui_unload_key` `d3dimgui_suppress_input` `d3dimgui_apply_dpi_scale` `d3dimgui_default_font_pixel` `d3dimgui_show_info` `d3dimgui_show_demo`
- 认目标与资源 `d3dimgui_max_targets` `d3dimgui_primary_area_ratio` `d3dimgui_stale_primary_frames` `d3dimgui_dummy_window_class` `d3dimgui_dummy_size` `d3dimgui_allow_warp` `d3dimgui_srv_slots`
- 兜底与交付 `d3dimgui_render_failure_fuse` `d3dimgui_rebind_fuse` `d3dimgui_enable_assert` `d3dimgui_start_with_console` `d3dimgui_log_to_file` `d3dimgui_crash_observer` `d3dimgui_self_unload`

框架不读 ini 也不写 `imgui.ini` 日志落 `%TEMP%\d3dimgui_layer.log` 是唯一往外写的文件 因为控制台会跟着进程一起没 而崩溃恰好在那一刻

## 实测过的标题

下面这些是真机注入过的 只说明"这套钩子和恢复逻辑在这几款上跑通了" 不是兼容性承诺 换版本换启动参数都可能变

- Minecraft 基岩版 走 opengl 那条 它压根不调 dxgi present 补 gl 后端之前注入进去一点图都没有
- Counter-Strike Global Offensive
- Escape The Backrooms
- Mecha Chameleon
- Shift At Midnight
- Ultimate Chicken Horse
- BrainBomb 演示版
- The NOexistenceN of you AND me

同一款游戏改启动参数就会换 api 所以不在这儿逐款标走哪条

## 目录

```
D3DImGuiLayer.slnx
x64\                         编译产物和 obj 缓存 不在项目目录里 也不进版本库
D3DImGuiLayer\
  include\d3d_imgui_layer\   d3dimgui.h 对外接口  d3dimgui_settings.h 编译期配置  d3dimgui_user.h 挂载点声明
  src\core\                  layer 生命周期与帧循环  log 崩溃现场  api 导出转发
  src\gfx\                   backend.h 接口  factory 按 api 分派  d3d11 d3d12 opengl 三个后端
  src\hook\                  vtable 采集  dxgi 钩子  opengl 钩子  slot_check.c 槽位编译期核死
  src\platform\              input 窗口子类化与光标  font 字体表
  src\user\draw.cpp          二次开发主落点
  third_party\               imgui 1.92.8 和 minhook 原样拷进来 不打补丁
```

没有 examples 工程 测试工具在仓库外的临时目录里 也不进版本库

## 已知边界

- 没有 d3d9 d3d10 vulkan opengl 是后补的那条 后端接口收的是 `void* surface` 交换链和 hdc 都塞得进去 再加 api 的成本在采集和呈现细节 不再是接口形状
- 只有 x64 不做 x86
- 交换链重建只在 d3d11 上有可重复用例 d3d12 只换链不换设备那种目前只是推理正确 没测 连窗口一起销毁重建同样没覆盖
- 两套 dxgi 实现并存这件事是处理了的 d3d11 和 d3d12 两条 dummy 链各认一份虚表 地址不同就分别下钩 上限两份 没做的是枚举那些两条链都没暴露出来的第三份实现 以及装完钩子若干秒 present 一次都没命中时自报进程里的渲染模块和前台窗口类名
- `d3dimgui_self_unload` 默认 0 detour 跑在宿主的调用栈上 还有线程站在我们的代码里就把镜像放掉是必崩 要真 unmap 自己打开并且清楚后果
- 联机反作弊环境不在这套代码的考虑范围内 那是封号问题不是渲染问题

## 更多文档

- `使用文档.md` 全量接口表 每个配置项的含义和什么时候该改 字体 输入 卸载 手动模式 常见问题
- `开发报告.md` 按轮次记录定位过的缺陷 证据 改法 以及走过弯路的那几处 首字节嗅探误判 dxgi 正常函数头 攥着宿主交换链引用把它的重建路堵死 以及在纯配置组合里才暴露的 gl 钩子装不上

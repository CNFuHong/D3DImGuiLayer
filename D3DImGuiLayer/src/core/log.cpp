#include "core/log.h"

#include <d3d_imgui_layer/d3dimgui_settings.h>

#include <TlHelp32.h>
#include <psapi.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace
{
using d3d_imgui_layer::log_error;

// 锁里不能再调 log 系列 会把这把锁二次拿起来 msvc 直接抛 system_error
std::recursive_mutex g_sink_mutex;
// 日志会在渲染线程 窗口线程和 hook 里同时打 这几个指针用原子量读
std::atomic<d3dimgui_log_fn> g_sink{nullptr};
std::atomic<void*> g_sink_user{nullptr};
std::atomic<HANDLE> g_console{INVALID_HANDLE_VALUE};
std::atomic<HANDLE> g_file{INVALID_HANDLE_VALUE};
bool g_console_ours = false;

const char* level_tag(const unsigned int level)
{
    switch (level)
    {
    case d3dimgui_log_warn:  return "[warn] ";
    case d3dimgui_log_error: return "[fail] ";
    default:                 return "[info] ";
    }
}

void console_write(const char* line, const int length)
{
    HANDLE handle = g_console.load(std::memory_order_acquire);
    if (handle == INVALID_HANDLE_VALUE || length <= 0)
        return;
    DWORD written = 0;
    WriteFile(handle, line, static_cast<DWORD>(length), &written, nullptr);
}

void file_write(const char* line, const int length)
{
    HANDLE handle = g_file.load(std::memory_order_acquire);
    if (handle == INVALID_HANDLE_VALUE || length <= 0)
        return;
    DWORD written = 0;
    WriteFile(handle, line, static_cast<DWORD>(length), &written, nullptr);
}

// 崩溃现场没有符号表也能定位 只要知道落在哪个模块的哪个偏移
void describe_address(const void* address, char* out, const size_t capacity)
{
    if (out == nullptr || capacity == 0)
        return;
    out[0] = '\0';
    if (address == nullptr)
        return;

    snprintf(out, capacity, "%p", address);

    HMODULE owner = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(address),
            &owner) == FALSE || owner == nullptr)
        return;

    wchar_t path[MAX_PATH] = {};
    if (GetModuleFileNameW(owner, path, MAX_PATH) == 0)
        return;

    const wchar_t* tail = path;
    for (const wchar_t* c = path; *c != L'\0'; ++c)
    {
        if (*c == L'\\' || *c == L'/')
            tail = c + 1;
    }

    char name[64] = {};
    WideCharToMultiByte(CP_UTF8, 0, tail, -1, name, static_cast<int>(sizeof(name)), nullptr, nullptr);

    const auto* base = reinterpret_cast<const unsigned char*>(owner);
    const auto* here = reinterpret_cast<const unsigned char*>(address);
    snprintf(out, capacity, "%s+0x%llx", name,
        static_cast<unsigned long long>(here - base));
}

bool fatal_code(const DWORD code)
{
    switch (code)
    {
    case EXCEPTION_ACCESS_VIOLATION:          // 0xc0000005
    case EXCEPTION_STACK_OVERFLOW:            // 0xc00000fd
    case EXCEPTION_ILLEGAL_INSTRUCTION:       // 0xc000001d
    case EXCEPTION_PRIV_INSTRUCTION:          // 0xc0000096
    case EXCEPTION_IN_PAGE_ERROR:             // 0xc0000006
    case 0xE06D7363:                          // msvc c++ 异常
    case 0xC0000409:                          // __fastfail 栈缓冲区检查
    case 0x80000003:                          // int3
        return true;
    default:
        return false;
    }
}

// 只登记我们模块内的返回地址 宿主自己的异常不打扰它
void scan_stack_for_self(CONTEXT* context, char* out, const size_t capacity)
{
    if (out == nullptr || capacity == 0)
        return;
    out[0] = '\0';

    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&fatal_code), &self);
    if (self == nullptr)
        return;

    MODULEINFO info{};
    if (K32GetModuleInformation(GetCurrentProcess(), self, &info, sizeof(info)) == FALSE)
        return;
    const auto base = reinterpret_cast<unsigned long long>(info.lpBaseOfDll);
    const auto top = base + info.SizeOfImage;

    unsigned long long* cursor = reinterpret_cast<unsigned long long*>(context->Rsp);
    size_t written = 0;
    int hits = 0;
    // 线性扫栈找返回地址 没有符号表也能看出是哪一层进的我
    for (int i = 0; i < 4096 && hits < 8; ++i)
    {
        unsigned long long value = 0;
        __try
        {
            value = cursor[i];
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            break;
        }

        if (value < base || value >= top)
            continue;

        const int used = snprintf(out + written, capacity - written, " self+0x%llx",
            value - base);
        if (used <= 0)
            break;
        written += static_cast<size_t>(used);
        if (written + 64 >= capacity)
            break;
        ++hits;
    }
}

LONG WINAPI crash_observer(EXCEPTION_POINTERS* pointers)
{
    if (pointers == nullptr || pointers->ExceptionRecord == nullptr)
        return EXCEPTION_CONTINUE_SEARCH;

    const DWORD code = pointers->ExceptionRecord->ExceptionCode;
    // 调试器和游戏自己的 seh 会大量用非致命异常 只记真的要死的
    if (!fatal_code(code))
        return EXCEPTION_CONTINUE_SEARCH;

    char where[96] = {};
    describe_address(pointers->ExceptionRecord->ExceptionAddress, where, sizeof(where));

    char stack[256] = {};
    if (pointers->ContextRecord != nullptr)
    {
        __try
        {
            scan_stack_for_self(pointers->ContextRecord, stack, sizeof(stack));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            stack[0] = '\0';
        }
    }

    const ULONG_PTR* params = pointers->ExceptionRecord->ExceptionInformation;
    const DWORD count = pointers->ExceptionRecord->NumberParameters;

    // 访问违例的前两个参数是读写标志和出错的地址 空指针加偏移一眼就能看出来
    if (code == EXCEPTION_ACCESS_VIOLATION && count >= 2)
    {
        char target[64] = {};
        describe_address(reinterpret_cast<const void*>(params[1]), target, sizeof(target));
        log_error("first chance access violation %s at %s target=%s stack=%s",
            params[0] == 0 ? "read" : params[0] == 1 ? "write" : "execute", where, target, stack);
    }
    else if (code == 0xE06D7363 && count >= 4)
    {
        // c++ 异常的参数里带着抛出对象的类型信息和抛出者的镜像基址
        // 能直接看出是谁 throw 的 上一轮 cs2 的崩溃就是靠这三个字段认出来的
        char thrower[96] = {};
        describe_address(reinterpret_cast<const void*>(params[3]), thrower, sizeof(thrower));
        log_error("first chance c++ exception code=0x%08lx at %s thrower=%s type=%p object=%p stack=%s",
            static_cast<unsigned long>(params[0]), where, thrower,
            reinterpret_cast<const void*>(params[2]), reinterpret_cast<const void*>(params[1]), stack);
    }
    else
    {
        char extra[128] = {};
        for (DWORD i = 0; i < count && i < 4; ++i)
        {
            const int used = snprintf(extra + static_cast<size_t>(strlen(extra)),
                sizeof(extra) - strlen(extra), " p%lu=0x%llx", i,
                static_cast<unsigned long long>(params[i]));
            if (used <= 0)
                break;
        }
        log_error("first chance code=0x%08lx addr=%s flags=0x%lx%s stack=%s", code, where,
            static_cast<unsigned long>(pointers->ExceptionRecord->ExceptionFlags), extra, stack);
    }

    return EXCEPTION_CONTINUE_SEARCH;
}
}

namespace d3d_imgui_layer
{
void set_logger(const d3dimgui_log_fn callback, void* user_data)
{
    g_sink.store(callback, std::memory_order_release);
    g_sink_user.store(user_data, std::memory_order_release);
}

// 注入进游戏后没有调试器也能看见日志 自己开一个控制台窗口
// 只回收自己 alloc 的那个 宿主本来有控制台的话不动它
void open_console(const bool open)
{
    std::lock_guard lock(g_sink_mutex);

    if (open)
    {
        if (g_console.load(std::memory_order_relaxed) != INVALID_HANDLE_VALUE)
            return;
        if (AllocConsole() == FALSE)
            return;
        g_console_ours = true;
        SetConsoleTitleA("d3dimgui layer log");
        g_console.store(CreateFileA("CONOUT$", GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr),
            std::memory_order_release);
        return;
    }

    const HANDLE previous = g_console.exchange(INVALID_HANDLE_VALUE, std::memory_order_acq_rel);
    if (previous != INVALID_HANDLE_VALUE)
        CloseHandle(previous);
    if (g_console_ours)
    {
        FreeConsole();
        g_console_ours = false;
    }
}

bool console_open()
{
    return g_console.load(std::memory_order_acquire) != INVALID_HANDLE_VALUE;
}

// 控制台跟着进程一起没 崩溃恰恰发生在进程要没的时候 所以诊断要落到文件里
void open_log_file(const bool open)
{
    std::lock_guard lock(g_sink_mutex);

    if (open)
    {
        if (g_file.load(std::memory_order_relaxed) != INVALID_HANDLE_VALUE)
            return;

        char path[MAX_PATH] = {};
        const DWORD length = GetTempPathA(MAX_PATH, path);
        if (length == 0 || length >= MAX_PATH)
            return;
        strncat_s(path, "d3dimgui_layer.log", _TRUNCATE);

        g_file.store(CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr), std::memory_order_release);
        if (g_file.load(std::memory_order_relaxed) != INVALID_HANDLE_VALUE)
            log_info("logging to %s", path);
        return;
    }

    const HANDLE previous = g_file.exchange(INVALID_HANDLE_VALUE, std::memory_order_acq_rel);
    if (previous != INVALID_HANDLE_VALUE)
        CloseHandle(previous);
}

void install_crash_observer()
{
    static bool installed = false;
    if (installed)
        return;
    installed = true;
    // 第一个进去 只为记现场 返回 continue search 不改任何行为
    AddVectoredExceptionHandler(1, &crash_observer);
}

void log_write(const unsigned int level, const char* format, va_list args)
{
    char body[512];
    const int length = vsnprintf(body, sizeof(body), format, args);

    char line[600];
    const int total = snprintf(line, sizeof(line), "d3dimgui %s%s\n", level_tag(level), body);

    file_write(line, total > 0 ? total : 0);

    const d3dimgui_log_fn sink = g_sink.load(std::memory_order_acquire);
    if (sink != nullptr)
        sink(level, body, g_sink_user.load(std::memory_order_acquire));
    else
        console_write(line, total > 0 ? total : 0);

    OutputDebugStringA(line);
}

void log_info(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    log_write(d3dimgui_log_info, format, args);
    va_end(args);
}

void log_warn(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    log_write(d3dimgui_log_warn, format, args);
    va_end(args);
}

void log_error(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    log_write(d3dimgui_log_error, format, args);
    va_end(args);
}
}

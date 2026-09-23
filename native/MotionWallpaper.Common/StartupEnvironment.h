#pragma once

#include "UniqueHandle.h"

#include <windows.h>
#include <tlhelp32.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace motion::startup
{
    namespace fs = std::filesystem;
    inline constexpr DWORD minimum_windows_build = 19045;

    struct EnvironmentResult
    {
        std::wstring error;
        std::wstring warning;
        explicit operator bool() const noexcept { return error.empty(); }
    };

    [[nodiscard]] inline std::wstring windows_error(DWORD code)
    {
        wchar_t text[1024]{};
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr, code, 0, text, ARRAYSIZE(text), nullptr);
        std::wstring message(text);
        while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n')) message.pop_back();
        return std::to_wstring(code) + (message.empty() ? L"" : L"（" + message + L"）");
    }

    [[nodiscard]] inline EnvironmentResult platform_requirement(
        DWORD major, DWORD build, WORD nativeMachine)
    {
        if (major < 10 || build < minimum_windows_build) {
            return { L"需要 Windows 10 22H2（内部版本 19045）或更新的 Windows。当前内部版本：" +
                std::to_wstring(build) + L"。请先更新系统后再启动。", {} };
        }
        if (nativeMachine == IMAGE_FILE_MACHINE_ARM64 && build >= 22000) {
            return { {}, L"当前是 Windows 11 ARM64，通过 x64 模拟运行；此路径尚未完成实机验证，视频驱动和解码兼容性可能不同。" };
        }
        if (nativeMachine != IMAGE_FILE_MACHINE_AMD64) {
            return { L"当前版本面向 x64 Windows。此系统架构不在已支持的运行范围内。", {} };
        }
        return {};
    }

    [[nodiscard]] inline EnvironmentResult check_environment()
    {
        using RtlGetVersionFunction = LONG(WINAPI*)(OSVERSIONINFOW*);
        auto versionFunction = reinterpret_cast<RtlGetVersionFunction>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
        OSVERSIONINFOW version{ sizeof(version) };
        if (!versionFunction || versionFunction(&version) != 0) {
            return { L"无法确认 Windows 系统版本；尚未启动后台服务。", {} };
        }
        USHORT processMachine{}, nativeMachine{};
        if (!IsWow64Process2(GetCurrentProcess(), &processMachine, &nativeMachine)) {
            return { L"无法确认系统架构：" + windows_error(GetLastError()), {} };
        }
        auto result = platform_requirement(version.dwMajorVersion, version.dwBuildNumber, nativeMachine);
        if (!result) return result;
        wchar_t systemDirectory[MAX_PATH + 1]{};
        auto length = GetSystemDirectoryW(systemDirectory, ARRAYSIZE(systemDirectory));
        if (!length || length >= ARRAYSIZE(systemDirectory)) {
            return { L"无法定位 Windows 系统媒体组件目录。", {} };
        }
        // Absolute system paths plus SYSTEM32-only dependency resolution never
        // load replacement media DLLs from the installation or current folder.
        struct Component { wchar_t const* name; char const* symbol; };
        constexpr Component components[]{
            { L"mfplat.dll", "MFStartup" },
            { L"mfreadwrite.dll", "MFCreateSourceReaderFromURL" },
            { L"mf.dll", "DllGetClassObject" },
            { L"mfmediaengine.dll", "DllGetClassObject" }
        };
        for (auto const& component : components) {
            auto path = fs::path(systemDirectory) / component.name;
            HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            DWORD error = module ? ERROR_SUCCESS : GetLastError();
            if (module && !GetProcAddress(module, component.symbol)) error = ERROR_PROC_NOT_FOUND;
            if (module) FreeLibrary(module);
            if (error) {
                result.error = L"Windows 媒体功能缺失或无法加载：" + path.wstring() +
                    L"\n系统错误：" + windows_error(error) +
                    L"\n如果使用 Windows N，请在“设置 → 可选功能 → 添加功能”安装“媒体功能包（Media Feature Pack）”，重启后再打开 Motion。" +
                    L"\n其他版本请修复系统媒体组件。程序不会自动安装系统功能，也无需从网上单独下载 DLL。";
                return result;
            }
        }
        return result;
    }

    [[nodiscard]] inline bool same_directory(fs::path const& left, fs::path const& right)
    {
        std::error_code error;
        auto a = fs::weakly_canonical(left, error);
        if (error) a = left.lexically_normal();
        error.clear();
        auto b = fs::weakly_canonical(right, error);
        if (error) b = right.lexically_normal();
        auto av = a.native(), bv = b.native();
        return CompareStringOrdinal(av.c_str(), static_cast<int>(av.size()),
            bv.c_str(), static_cast<int>(bv.size()), TRUE) == CSTR_EQUAL;
    }

    struct RunningProcess
    {
        DWORD id{};
        fs::path executable;
        bool agent{};
    };

    struct InstallationState
    {
        std::vector<RunningProcess> processes;
        std::wstring conflict;
    };

    [[nodiscard]] inline std::optional<fs::path> process_image(HANDLE process)
    {
        std::wstring image(32768, L'\0');
        DWORD length = static_cast<DWORD>(image.size());
        if (!QueryFullProcessImageNameW(process, 0, image.data(), &length)) return {};
        image.resize(length);
        return fs::path(image);
    }

    [[nodiscard]] inline InstallationState inspect_installation(fs::path const& directory)
    {
        InstallationState result;
        DWORD currentSession{};
        if (!ProcessIdToSessionId(GetCurrentProcessId(), &currentSession)) {
            result.conflict = L"无法确认当前登录会话：" + windows_error(GetLastError());
            return result;
        }
        unique_handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
        if (!snapshot) {
            result.conflict = L"无法检查正在运行的 Motion 实例：" + windows_error(GetLastError());
            return result;
        }
        PROCESSENTRY32W entry{ sizeof(entry) };
        if (!Process32FirstW(snapshot.get(), &entry)) {
            result.conflict = L"无法枚举正在运行的 Motion 实例：" + windows_error(GetLastError());
            return result;
        }
        do {
            bool agent = _wcsicmp(entry.szExeFile, L"motionwallpaper-agent.exe") == 0;
            if ((!agent && _wcsicmp(entry.szExeFile, L"MotionWallpaper.exe") != 0) ||
                entry.th32ProcessID == GetCurrentProcessId()) continue;
            DWORD session{};
            if (!ProcessIdToSessionId(entry.th32ProcessID, &session)) {
                DWORD error = GetLastError();
                if (error == ERROR_INVALID_PARAMETER) continue;
                result.conflict = L"无法确认已运行 Motion 进程所属会话（PID " +
                    std::to_wstring(entry.th32ProcessID) + L"）：" + windows_error(error) +
                    L"。请先退出该实例后再打开本程序。";
                return result;
            }
            if (session != currentSession) continue;
            unique_handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, entry.th32ProcessID));
            if (!process) {
                DWORD error = GetLastError();
                if (error == ERROR_INVALID_PARAMETER) continue; // It exited after the snapshot.
                result.conflict = L"无法确认已运行 Motion 进程的安装位置（PID " +
                    std::to_wstring(entry.th32ProcessID) + L"）：" + windows_error(error) +
                    L"。请先退出该实例后再打开本程序。";
                return result;
            }
            if (WaitForSingleObject(process.get(), 0) == WAIT_OBJECT_0) continue;
            auto image = process_image(process.get());
            if (!image) {
                result.conflict = L"无法读取已运行 Motion 进程的路径（PID " +
                    std::to_wstring(entry.th32ProcessID) + L"）。请先退出该实例后再打开本程序。";
                return result;
            }
            if (!same_directory(image->parent_path(), directory)) {
                result.conflict = L"另一安装位置的 Motion 正在运行：\n" + image->wstring() +
                    L"\n\n本次启动位置：\n" + directory.wstring() +
                    L"\n\n请先从系统托盘退出正在运行的 Motion（包括后台服务），再打开本位置的程序。为避免混用设置，本次启动已停止，不会向另一实例发送指令。";
                return result;
            }
            result.processes.push_back({ entry.th32ProcessID, std::move(*image), agent });
        } while (Process32NextW(snapshot.get(), &entry));
        return result;
    }

    [[nodiscard]] inline DWORD probe_directory_write(fs::path directory)
    {
        std::error_code error;
        while (!fs::exists(directory, error)) {
            if (error) return static_cast<DWORD>(error.value());
            auto parent = directory.parent_path();
            if (parent.empty() || parent == directory) return ERROR_PATH_NOT_FOUND;
            directory = std::move(parent);
        }
        if (error) return static_cast<DWORD>(error.value());
        static std::atomic_uint64_t sequence{};
        for (unsigned attempt = 0; attempt < 16; ++attempt) {
            auto name = L".motion-write-check-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(++sequence) + L".tmp";
            unique_handle file(CreateFileW((directory / name).c_str(), GENERIC_WRITE | DELETE,
                0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr));
            if (!file) {
                DWORD status = GetLastError();
                if (status == ERROR_FILE_EXISTS || status == ERROR_ALREADY_EXISTS) continue;
                return status;
            }
            char byte{};
            DWORD written{};
            if (!WriteFile(file.get(), &byte, sizeof(byte), &written, nullptr)) return GetLastError();
            return written == sizeof(byte) ? ERROR_SUCCESS : ERROR_WRITE_FAULT;
        }
        return ERROR_FILE_EXISTS;
    }

    [[nodiscard]] inline std::wstring check_data_directory(fs::path const& root)
    {
        for (auto const& directory : { root, root / L"Config" }) {
            DWORD error = probe_directory_write(directory);
            if (error) return L"Motion 数据目录不可写或磁盘不可用：\n" + directory.wstring() +
                L"\n系统错误：" + windows_error(error) +
                L"\n请检查目录写入权限、磁盘空间和只读状态后重新打开程序。设置文件尚未加载或覆盖，存储位置没有改变。";
        }
        DWORD attributes = GetFileAttributesW((root / L"Config" / L"settings.json").c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY)) {
            return L"设置文件为只读：\n" + (root / L"Config" / L"settings.json").wstring() +
                L"\n请取消只读或恢复该目录写入权限后重新打开。原文件未修改。";
        }
        return {};
    }

    [[nodiscard]] inline bool runtime_belongs_to_process(HANDLE process, fs::path const& runtimePath)
    {
        FILETIME created{}, exited{}, kernel{}, user{};
        WIN32_FILE_ATTRIBUTE_DATA file{};
        return GetProcessTimes(process, &created, &exited, &kernel, &user) &&
            GetFileAttributesExW(runtimePath.c_str(), GetFileExInfoStandard, &file) &&
            CompareFileTime(&file.ftLastWriteTime, &created) >= 0;
    }

    struct AgentStartResult
    {
        bool ready{};
        bool cancelled{};
        DWORD processId{};
        DWORD systemError{};
        std::optional<DWORD> exitCode;
        std::wstring message;
    };

    // Called on a worker thread. Readiness requires a live matching process,
    // its own runtime identity, and a publication newer than process creation.
    template<typename ReadHandshake, typename Cancelled>
    [[nodiscard]] AgentStartResult start_or_connect_agent(fs::path const& directory,
        fs::path const& runtimePath, ReadHandshake const& readHandshake,
        Cancelled const& cancelled, std::chrono::milliseconds timeout = std::chrono::seconds(10),
        std::function<InstallationState(fs::path const&)> const& inspect = inspect_installation)
    {
        AgentStartResult result;
        if (cancelled()) { result.cancelled = true; return result; }
        auto installation = inspect(directory);
        if (!installation.conflict.empty()) { result.message = installation.conflict; return result; }
        unique_handle process;
        for (auto const& candidate : installation.processes) {
            if (!candidate.agent) continue;
            process.reset(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, candidate.id));
            if (process && WaitForSingleObject(process.get(), 0) == WAIT_TIMEOUT) {
                result.processId = candidate.id;
                break;
            }
            process.reset();
        }
        if (!process) {
            auto executable = directory / L"motionwallpaper-agent.exe";
            std::wstring command = L"\"" + executable.wstring() + L"\"";
            STARTUPINFOW startup{ sizeof(startup) };
            PROCESS_INFORMATION created{};
            if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                    CREATE_NO_WINDOW, nullptr, directory.c_str(), &startup, &created)) {
                result.systemError = GetLastError();
                result.message = L"无法启动后台服务：\n" + executable.wstring() +
                    L"\n系统错误：" + windows_error(result.systemError);
                return result;
            }
            unique_handle thread(created.hThread);
            process.reset(created.hProcess);
            result.processId = created.dwProcessId;
        }
        auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            if (cancelled()) { result.cancelled = true; return result; }
            DWORD wait = WaitForSingleObject(process.get(), 0);
            if (wait == WAIT_OBJECT_0) {
                DWORD code{};
                if (GetExitCodeProcess(process.get(), &code)) result.exitCode = code;
                // Another same-install process can win the Agent mutex while
                // this process is starting. Connect to that winner once found.
                auto running = inspect(directory);
                if (!running.conflict.empty()) { result.message = running.conflict; return result; }
                bool connected{};
                for (auto const& candidate : running.processes) {
                    if (!candidate.agent || candidate.id == result.processId) continue;
                    unique_handle replacement(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
                        FALSE, candidate.id));
                    if (!replacement || WaitForSingleObject(replacement.get(), 0) != WAIT_TIMEOUT) continue;
                    process = std::move(replacement);
                    result.processId = candidate.id;
                    result.exitCode.reset();
                    connected = true;
                    break;
                }
                if (connected) continue;
                wchar_t exitText[32]{};
                if (result.exitCode) swprintf_s(exitText, L"0x%08lX", *result.exitCode);
                result.message = L"后台服务在就绪前退出（PID " + std::to_wstring(result.processId) +
                    L"），退出码：" + (result.exitCode ? std::wstring(exitText) : L"无法读取") +
                    L"。\n请查看 " + (runtimePath.parent_path() / L"agent.log").wstring();
                return result;
            }
            if (wait == WAIT_FAILED) {
                result.systemError = GetLastError();
                result.message = L"无法查询后台服务状态：" + windows_error(result.systemError);
                return result;
            }
            if (runtime_belongs_to_process(process.get(), runtimePath) && readHandshake(result.processId)) {
                result.ready = true;
                return result;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                result.message = L"后台服务已创建，但未在 " + std::to_wstring(timeout.count() / 1000) +
                    L" 秒内确认就绪（PID " +
                    std::to_wstring(result.processId) + L"）。\n安装位置：" + directory.wstring() +
                    L"\n请查看 " + (runtimePath.parent_path() / L"agent.log").wstring() +
                    L"。可以稍后点击“重试”；程序没有强制结束正在运行的服务。";
                return result;
            }
            WaitForSingleObject(process.get(), 100);
        }
    }
}

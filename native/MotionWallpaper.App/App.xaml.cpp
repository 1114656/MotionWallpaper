#include "pch.h"
#include "App.xaml.h"
#include "MainWindow.xaml.h"
#include "../MotionWallpaper.Common/StartupEnvironment.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace
{
    [[noreturn]] void stop_startup(std::wstring const& message, UINT code = 1)
    {
        MessageBoxW(nullptr, message.c_str(), L"MotionWallpaper 无法启动", MB_OK | MB_ICONERROR);
        ExitProcess(code);
    }

    bool activate_settings_window(DWORD processId)
    {
        struct Search { DWORD processId; HWND window{}; } search{ processId };
        EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
            auto& found = *reinterpret_cast<Search*>(parameter);
            DWORD owner{};
            GetWindowThreadProcessId(window, &owner);
            if (owner != found.processId || !IsWindowVisible(window)) return TRUE;
            wchar_t title[64]{};
            GetWindowTextW(window, title, ARRAYSIZE(title));
            if (wcscmp(title, L"MotionWallpaper") != 0) return TRUE;
            found.window = window;
            return FALSE;
        }, reinterpret_cast<LPARAM>(&search));
        if (!search.window) return false;
        ShowWindow(search.window, SW_RESTORE);
        SetForegroundWindow(search.window);
        return true;
    }
}

namespace winrt::MotionWallpaper::implementation
{
    App::App()
    {
        auto applicationRoot = motion::executable_directory();
        auto installation = motion::startup::inspect_installation(applicationRoot);
        if (!installation.conflict.empty()) stop_startup(installation.conflict);
        instanceMutex.reset(CreateMutexW(nullptr, FALSE, L"Local\\MotionWallpaper.SettingsApp"));
        DWORD mutexStatus = GetLastError();
        if (!instanceMutex) stop_startup(L"无法创建应用实例锁：" + motion::startup::windows_error(mutexStatus));
        if (mutexStatus == ERROR_ALREADY_EXISTS) {
            for (int attempt = 0; attempt < 20; ++attempt) {
                installation = motion::startup::inspect_installation(applicationRoot);
                if (!installation.conflict.empty()) stop_startup(installation.conflict);
                for (auto const& process : installation.processes) {
                    if (!process.agent && activate_settings_window(process.id)) ExitProcess(0);
                }
                Sleep(50);
            }
            ExitProcess(0);
        }
        // This precedes MainWindow, SettingsStore::Load, restore recovery, and
        // global named-event notifications. A blocked launch leaves data alone.
        auto environment = motion::startup::check_environment();
        if (!environment) stop_startup(environment.error);
        if (!motion::legacy_data_conflict_present(applicationRoot)) {
            auto writeError = motion::startup::check_data_directory(motion::application_data_directory());
            if (!writeError.empty()) stop_startup(writeError);
        }
        if (!environment.warning.empty()) {
            MessageBoxW(nullptr, environment.warning.c_str(), L"MotionWallpaper 运行环境", MB_OK | MB_ICONINFORMATION);
        }
        InitializeComponent();
    }

    App::~App()
    {
        if (exitStopEvent) SetEvent(exitStopEvent.get());
        if (exitWatcher.joinable()) exitWatcher.join();
    }

    void App::OnLaunched(LaunchActivatedEventArgs const&)
    {
        auto installation = motion::startup::inspect_installation(motion::executable_directory());
        if (!installation.conflict.empty()) stop_startup(installation.conflict);
        window = make<MainWindow>();
        window.Activate();

        exitEvent.reset(CreateEventW(nullptr, TRUE, FALSE, motion::app_exit_event_name));
        exitStopEvent.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!exitEvent || !exitStopEvent) return;
        auto dispatcher = Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
        exitWatcher = std::thread([this, dispatcher] {
            HANDLE events[]{ exitStopEvent.get(), exitEvent.get() };
            if (WaitForMultipleObjects(ARRAYSIZE(events), events, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) return;
            dispatcher.TryEnqueue([this] {
                if (!window) return;
                window.Close();
                window = nullptr;
            });
        });
    }
}

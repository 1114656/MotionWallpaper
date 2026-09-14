#include "pch.h"
#include "MainWindow.xaml.h"
#include "LibraryMigration.h"
#include "resource.h"
#include "VariantTaskView.h"

#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
namespace fs = std::filesystem;

namespace
{
    constexpr uint8_t variant_source = 1;
    constexpr uint8_t variant_balanced = 2;
    constexpr uint8_t variant_power_saver = 4;

    int combo_int(ComboBox const& box, int fallback)
    {
        auto item = box.SelectedItem().try_as<ComboBoxItem>();
        if (!item) return fallback;
        auto value = unbox_value_or<hstring>(item.Tag(), {});
        return value.empty() ? fallback : _wtoi(value.c_str());
    }

    std::string combo_string(ComboBox const& box, std::string const& fallback)
    {
        auto item = box.SelectedItem().try_as<ComboBoxItem>();
        if (!item) return fallback;
        auto value = unbox_value_or<hstring>(item.Tag(), {});
        return value.empty() ? fallback : motion::wide_to_utf8(value.c_str());
    }

    void select_tag(ComboBox const& box, std::wstring const& tag)
    {
        for (uint32_t index = 0; index < box.Items().Size(); ++index) {
            auto item = box.Items().GetAt(index).try_as<ComboBoxItem>();
            if (item && unbox_value_or<hstring>(item.Tag(), {}) == tag) {
                box.SelectedIndex(static_cast<int32_t>(index));
                return;
            }
        }
        box.SelectedIndex(0);
    }

    std::wstring format_size(uint64_t bytes)
    {
        static wchar_t const* units[]{ L"B", L"KB", L"MB", L"GB", L"TB" };
        double value = static_cast<double>(bytes);
        size_t unit = 0;
        while (value >= 1024.0 && unit < ARRAYSIZE(units) - 1) { value /= 1024.0; ++unit; }
        wchar_t output[64]{};
        swprintf_s(output, unit > 1 ? L"%.1f %s" : L"%.0f %s", value, units[unit]);
        return output;
    }

    std::wstring file_uri(fs::path const& path) { return L"file:///" + path.generic_wstring(); }

    void append_fingerprint(std::wstring& output, std::wstring_view value)
    {
        output += std::to_wstring(value.size());
        output.push_back(L':');
        output.append(value);
        output.push_back(L'|');
    }

    void append_fingerprint(std::wstring& output, std::string_view value)
    {
        output += std::to_wstring(value.size());
        output.push_back(L':');
        for (auto character : value) output.push_back(static_cast<unsigned char>(character));
        output.push_back(L'|');
    }

    void append_fingerprint(std::wstring& output, uint64_t value)
    {
        append_fingerprint(output, std::to_wstring(value));
    }

    std::wstring variant_page_structure_fingerprint(
        std::vector<motion::app::VariantMediaSummary> const& items, bool waitingForPower)
    {
        std::wstring output;
        output.reserve(items.size() * 160);
        append_fingerprint(output, static_cast<uint64_t>(waitingForPower));
        for (auto const& item : items) {
            append_fingerprint(output, item.media.id);
            append_fingerprint(output, item.media.name);
            append_fingerprint(output, item.groupName);
            append_fingerprint(output, item.media.coverFileName);
            append_fingerprint(output, item.media.sizeBytes);
            append_fingerprint(output, static_cast<uint64_t>(item.sourceAvailable));
            append_fingerprint(output, item.status.requestedMode);
            append_fingerprint(output, static_cast<uint64_t>(item.status.queued));
            append_fingerprint(output, static_cast<uint64_t>(item.status.generating));
            append_fingerprint(output, static_cast<uint64_t>(item.status.paused));
            append_fingerprint(output, static_cast<uint64_t>(item.status.waitingForPower));
            // Progress values are updated on the existing task controls. Keep
            // them out of the structure fingerprint so a one-percent change
            // does not rebuild every media card or discard keyboard/UIA focus.
            append_fingerprint(output, static_cast<uint64_t>(item.status.cancelled));
            append_fingerprint(output, static_cast<uint64_t>(item.status.failed));
            append_fingerprint(output, item.status.failedMode);
            append_fingerprint(output, static_cast<uint64_t>(item.status.balancedSuppressed));
            append_fingerprint(output, static_cast<uint64_t>(item.status.powerSaverSuppressed));
            append_fingerprint(output, item.balanced.files);
            append_fingerprint(output, item.balanced.bytes);
            append_fingerprint(output, static_cast<uint64_t>(item.balanced.sharedStorage));
            append_fingerprint(output, item.powerSaver.files);
            append_fingerprint(output, item.powerSaver.bytes);
            append_fingerprint(output, static_cast<uint64_t>(item.powerSaver.sharedStorage));
        }
        return output;
    }

    struct VariantSelectionControls
    {
        winrt::weak_ref<CheckBox> source;
        winrt::weak_ref<CheckBox> balanced;
        winrt::weak_ref<CheckBox> powerSaver;
        winrt::weak_ref<Button> remove;
        std::wstring mediaName;
        uint8_t available{};
    };

    void update_variant_selection_controls(
        std::shared_ptr<VariantSelectionControls> const& controls, uint8_t selection)
    {
        selection &= controls->available;
        auto setChecked = [](winrt::weak_ref<CheckBox> const& weak, bool checked) {
            if (auto box = weak.get()) {
                box.IsChecked(box_value(checked).as<Windows::Foundation::IReference<bool>>());
            }
        };
        setChecked(controls->source, selection & variant_source);
        setChecked(controls->balanced, selection & variant_balanced);
        setChecked(controls->powerSaver, selection & variant_power_saver);
        if (auto remove = controls->remove.get()) {
            auto label = selection & variant_source
                ? std::wstring(L"删除源文件")
                : selection ? std::wstring(L"删除所选") : std::wstring(L"先选择");
            remove.Content(box_value(label));
            Automation::AutomationProperties::SetName(
                remove, hstring(controls->mediaName + L"，" + label));
            remove.IsEnabled(selection != 0);
        }
    }

    void set_media_card_selected(GridViewItem const& card, bool selected)
    {
        auto content = card.Content().try_as<StackPanel>();
        if (!content || content.Children().Size() == 0) return;
        auto preview = content.Children().GetAt(0).try_as<Border>();
        if (!preview) return;
        auto previewContent = preview.Child().try_as<Grid>();
        if (!previewContent) return;

        static hstring const selectionBadgeTag{ L"media-selection-badge" };
        for (uint32_t index = previewContent.Children().Size(); index > 0; --index) {
            auto element = previewContent.Children().GetAt(index - 1).try_as<FrameworkElement>();
            if (element && unbox_value_or<hstring>(element.Tag(), {}) == selectionBadgeTag) {
                previewContent.Children().RemoveAt(index - 1);
            }
        }

        if (!selected) {
            preview.BorderBrush(nullptr);
            preview.BorderThickness(ThicknessHelper::FromUniformLength(0));
            return;
        }

        auto accent = Microsoft::UI::Xaml::Media::SolidColorBrush{
            Windows::UI::ColorHelper::FromArgb(255, 10, 115, 232) };
        preview.BorderBrush(accent);
        preview.BorderThickness(ThicknessHelper::FromUniformLength(2));
        Border badge;
        badge.Tag(box_value(selectionBadgeTag));
        badge.Width(24);
        badge.Height(24);
        badge.CornerRadius(CornerRadiusHelper::FromUniformRadius(12));
        badge.Background(accent);
        badge.HorizontalAlignment(HorizontalAlignment::Right);
        badge.VerticalAlignment(VerticalAlignment::Top);
        badge.Margin(ThicknessHelper::FromLengths(0, 8, 8, 0));
        FontIcon check;
        check.Glyph(L"\xE73E");
        check.FontSize(12);
        check.Foreground(Microsoft::UI::Xaml::Media::SolidColorBrush{ Windows::UI::Colors::White() });
        badge.Child(check);
        previewContent.Children().Append(badge);
    }

    std::vector<fs::path> select_files(HWND owner, wchar_t const* title, wchar_t const* pattern)
    {
        com_ptr<IFileOpenDialog> dialog;
        check_hresult(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put())));
        DWORD options{};
        check_hresult(dialog->GetOptions(&options));
        check_hresult(dialog->SetOptions(options | FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM));
        COMDLG_FILTERSPEC filters[]{ { title, pattern }, { L"所有文件", L"*.*" } };
        check_hresult(dialog->SetFileTypes(ARRAYSIZE(filters), filters));
        auto result = dialog->Show(owner);
        if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return {};
        check_hresult(result);
        com_ptr<IShellItemArray> items;
        check_hresult(dialog->GetResults(items.put()));
        DWORD count{};
        check_hresult(items->GetCount(&count));
        std::vector<fs::path> paths;
        for (DWORD index = 0; index < count; ++index) {
            com_ptr<IShellItem> item;
            check_hresult(items->GetItemAt(index, item.put()));
            PWSTR path{};
            check_hresult(item->GetDisplayName(SIGDN_FILESYSPATH, &path));
            paths.emplace_back(path);
            CoTaskMemFree(path);
        }
        return paths;
    }

    fs::path select_folder(HWND owner, wchar_t const* title)
    {
        com_ptr<IFileOpenDialog> dialog;
        check_hresult(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put())));
        DWORD options{};
        check_hresult(dialog->GetOptions(&options));
        check_hresult(dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM));
        check_hresult(dialog->SetTitle(title));
        auto result = dialog->Show(owner);
        if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return {};
        check_hresult(result);
        com_ptr<IShellItem> item;
        check_hresult(dialog->GetResult(item.put()));
        PWSTR path{};
        check_hresult(item->GetDisplayName(SIGDN_FILESYSPATH, &path));
        fs::path selected(path);
        CoTaskMemFree(path);
        return selected;
    }

}

namespace winrt::MotionWallpaper::implementation
{
    MainWindow::MainWindow()
    {
        InitializeComponent();
        Title(L"MotionWallpaper");
        try { SystemBackdrop(Microsoft::UI::Xaml::Media::MicaBackdrop{}); } catch (...) {}
        HWND window{};
        auto nativeWindow = this->try_as<::IWindowNative>();
        check_hresult(nativeWindow->get_WindowHandle(&window));
        auto instance = GetModuleHandleW(nullptr);
        auto largeIcon = static_cast<HICON>(LoadImageW(
            instance, MAKEINTRESOURCEW(IDI_MOTIONWALLPAPER), IMAGE_ICON,
            GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_SHARED));
        auto smallIcon = static_cast<HICON>(LoadImageW(
            instance, MAKEINTRESOURCEW(IDI_MOTIONWALLPAPER), IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
        if (largeIcon) SendMessageW(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(largeIcon));
        if (smallIcon) SendMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(smallIcon));
        UINT dpi = GetDpiForWindow(window);
        int width = MulDiv(1440, static_cast<int>(dpi), 96);
        int height = MulDiv(1024, static_cast<int>(dpi), 96);
        MONITORINFO monitor{ sizeof(monitor) };
        GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
        width = (std::min)(width, static_cast<int>(monitor.rcWork.right - monitor.rcWork.left));
        height = (std::min)(height, static_cast<int>(monitor.rcWork.bottom - monitor.rcWork.top));
        int x = monitor.rcWork.left + ((monitor.rcWork.right - monitor.rcWork.left) - width) / 2;
        int y = monitor.rcWork.top + ((monitor.rcWork.bottom - monitor.rcWork.top) - height) / 2;
        SetWindowPos(window, nullptr, x, y, width, height, SWP_NOACTIVATE | SWP_NOZORDER);

        auto dispatcher = Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
        settingsSaveTimer = dispatcher.CreateTimer();
        settingsSaveTimer.Interval(std::chrono::milliseconds(350));
        settingsSaveTimer.IsRepeating(false);
        settingsSaveTimer.Tick([this](auto const&, auto const&) { TrySaveSettings(); });
        statusHideTimer = dispatcher.CreateTimer();
        statusHideTimer.Interval(std::chrono::seconds(2));
        statusHideTimer.IsRepeating(false);
        statusHideTimer.Tick([this](auto const&, auto const&) { StatusBar().IsOpen(false); });
        settingsReloadTimer = dispatcher.CreateTimer();
        settingsReloadTimer.Interval(std::chrono::seconds(1));
        settingsReloadTimer.IsRepeating(true);
        settingsReloadTimer.Tick([this](auto const&, auto const&) {
            ReloadExternalSelection();
            if (optimizationWorkVisible && currentPage == AppPage::Variants) RefreshVariants();
        });
        Closed([this](auto const&, auto const&) {
            closing.store(true, std::memory_order_release);
            if (importCancellation) importCancellation->store(true, std::memory_order_release);
            if (libraryMigrationCancellation) libraryMigrationCancellation->store(true, std::memory_order_release);
            if (activeLibraryMigrationPause) activeLibraryMigrationPause->Cancel();
            settingsSaveTimer.Stop();
            statusHideTimer.Stop();
            settingsReloadTimer.Stop();
            if (!initializing && settingsStore) TrySaveSettings();
        });

        applicationRoot = motion::executable_directory();
        bool legacyDataConflict = motion::legacy_data_conflict_present(applicationRoot);
        // A migration conflict deliberately has no authoritative side. Keep a
        // path only for UI construction, but do not inspect either data tree.
        root = legacyDataConflict ? applicationRoot : motion::application_data_directory();
        settingsStore = std::make_unique<motion::app::SettingsStore>(root, applicationRoot);
        try {
            if (legacyDataConflict) {
                settingsWritable = false;
                mediaLibraryAvailable = false;
            } else {
                settings = settingsStore->Load(&mediaLibraryAvailable);
            }
        } catch (...) {
            settingsWritable = false;
            mediaLibraryAvailable = false;
            ShowStatus(L"设置文件无法读取，已使用安全默认值；原文件未被覆盖。", true);
        }
        fs::path configuredLibraryPath;
        try {
            configuredLibraryPath = legacyDataConflict
                ? (root / L"Wallpapers").lexically_normal()
                : motion::wallpaper_library_directory(root, settings.mediaLibraryPath);
        } catch (...) {
            settingsWritable = false;
            mediaLibraryAvailable = false;
            configuredLibraryPath = (root / L"Wallpapers").lexically_normal();
            ShowStatus(L"媒体库路径无效，已进入安全只读状态；原设置和目标路径均未修改。", true);
        }
        // An unreadable/future settings document may contain a library path
        // this binary does not understand. Stay fully read-only instead of
        // creating or repairing a different default library behind the user's
        // back; once the settings file is repaired, a restart resumes normally.
        if (settingsWritable && mediaLibraryAvailable) {
            if (settings.mediaLibraryPath.empty()) {
                // The application-owned default remains the compatible
                // first-start path and may be created after construction.
            } else {
                // SettingsStore has performed the full owned-tree validation.
                // Bind that result to stable direct object identities so every
                // later operation can cheaply detect drive/root replacement.
                auto identity = motion::capture_media_library_trust(
                    configuredLibraryPath);
                if (!identity || identity->ownershipId != settings.mediaLibraryId ||
                    !motion::is_owned_media_library(identity->root) ||
                    !motion::revalidate_media_library_trust(*identity)) {
                    mediaLibraryAvailable = false;
                } else {
                    mediaLibraryTrust = std::move(*identity);
                }
            }
        }
        try {
            mediaLibrary = std::make_shared<motion::app::MediaLibrary>(root,
                motion::app::DeleteMode::RecycleBin, configuredLibraryPath,
                mediaLibraryTrust);
        } catch (...) {
            if (settings.mediaLibraryPath.empty()) throw;
            mediaLibraryTrust.reset();
            mediaLibraryAvailable = false;
            mediaLibrary = std::make_shared<motion::app::MediaLibrary>(root,
                motion::app::DeleteMode::RecycleBin, configuredLibraryPath);
        }
        if (settingsWritable && mediaLibraryAvailable &&
            settings.mediaLibraryPath.empty()) {
            mediaLibrary->EnsureDirectories();
        }
        motion::RuntimeState runtime;
        if (!legacyDataConflict &&
            motion::try_load_runtime(root / L"Config" / L"runtime.json", runtime)) {
            appliedGroupId = std::move(runtime.activeGroupId);
            appliedMediaId = std::move(runtime.activeMediaId);
            actualDecodePath = std::move(runtime.decodePath);
            actualDecodeReason = std::move(runtime.decodeReason);
        } else {
            appliedGroupId = settings.selectedGroupId;
            appliedMediaId = settings.selectedMediaId;
        }
        std::error_code runtimeTimeError;
        runtimeWriteTime = fs::last_write_time(root / L"Config" / L"runtime.json", runtimeTimeError);
        ApplySettingsToControls();
        LoadDisplayTargets();
        LoadGroups();
        initializing = false;
        LoadMedia();
        ShowSettingsPage();
        auto const libraryPath = mediaLibrary->WallpapersPath().wstring();
        LibraryPath().Text(libraryPath);
        LibraryPathFull().Text(libraryPath);
        if (legacyDataConflict) {
            ShowStatus(L"安装数据迁移检测到旧库与新库冲突；两侧均未读写，媒体功能已安全停用。请先完成恢复后重启应用。", true);
        } else if (settingsWritable && !mediaLibraryAvailable) {
            ShowStatus(L"自定义媒体库当前不可访问或所有权校验失败；已停用所有媒体读写。连接原磁盘后可重试迁移位置。", true);
        }
        settingsReloadTimer.Start();
        if (!motion::notify_settings_changed()) StartController();
    }

    void MainWindow::SaveSettings()
    {
        if (!settingsWritable) {
            throw std::runtime_error("settings are read-only after an unsupported or corrupt load");
        }
        if (!settingsStore->Save(settings)) StartController();
    }

    bool MainWindow::TrySaveSettings() noexcept
    {
        try {
            SaveSettings();
            return true;
        } catch (...) {
            ShowStatus(L"无法保存设置，请确认用户数据目录可写且配置文件未被占用。", true);
            return false;
        }
    }

    std::shared_ptr<motion::app::LibraryWriteLease> MainWindow::TryAcquireLibraryWrite(bool showError)
    {
        if (!settingsWritable) {
            if (showError) {
                ShowStatus(L"当前设置文件无法安全读取，媒体库以只读方式停用；请修复设置文件后重启应用。", true);
            }
            return {};
        }
        if (!mediaLibraryAvailable) {
            if (showError) {
                ShowStatus(L"自定义媒体库当前离线或未通过所有权校验；不会读取、创建或修改该路径。", true);
            }
            return {};
        }
        auto lease = libraryAccessGate->TryAcquireWrite();
        if (!lease) {
            if (showError) ShowStatus(L"媒体库正在迁移，请等待迁移完成后再操作。", true);
            return {};
        }
        if (!settings.mediaLibraryPath.empty()) {
            auto trust = mediaLibraryTrust &&
                mediaLibraryTrust->ownershipId == settings.mediaLibraryId
                ? motion::acquire_media_library_trust(*mediaLibraryTrust)
                : std::shared_ptr<motion::MediaLibraryTrustLease>{};
            if (!trust) {
                // Keep the settings path intact for recovery, but make the
                // current process fail closed until the user reconnects a fully
                // validated owned library.
                lease.reset();
                mediaLibraryTrust.reset();
                mediaLibraryAvailable = false;
                if (!motion::notify_settings_changed()) StartController();
                if (showError) {
                    ShowStatus(L"媒体库磁盘、目录身份或所有权标记已变化；已立即停止所有媒体访问。请连接已有的有效媒体库。", true);
                }
                return {};
            }
            lease->RetainMediaLibraryTrust(*mediaLibraryTrust, std::move(trust));
        }
        return lease;
    }

    void MainWindow::SetLibraryMigrationUi(bool migrating)
    {
        // Destructive library operations keep a rollback snapshot of settings while
        // the Agent drains renderers and transcoders.  Disable every page (including
        // the currently focused control) so a later rollback cannot overwrite a
        // setting the user changed during that acknowledgement window.
        Content().as<FrameworkElement>().IsHitTestVisible(!migrating);
        SettingsPage().IsEnabled(!migrating);
        VariantsPage().IsEnabled(!migrating);
        WallpaperPage().IsEnabled(!migrating);
        SettingsNavButton().IsEnabled(!migrating);
        VariantsNavButton().IsEnabled(!migrating);
        AddGroupButton().IsEnabled(!migrating);
        NewGroupName().IsEnabled(!migrating);
        GroupPicker().IsEnabled(!migrating);
        MediaList().IsEnabled(!migrating);
        ImportImageButton().IsEnabled(!migrating);
        ImportVideoButton().IsEnabled(!migrating);
        if (migrating) {
            DeleteMediaButton().IsEnabled(false);
            RenameMediaButton().IsEnabled(false);
            MoveMediaButton().IsEnabled(false);
        } else {
            UpdateMediaActionState();
        }
    }

    void MainWindow::ReloadExternalSelection()
    {
        auto path = root / L"Config" / L"runtime.json";
        std::error_code error;
        auto writeTime = fs::last_write_time(path, error);
        if (error || writeTime == runtimeWriteTime) return;
        runtimeWriteTime = writeTime;

        motion::RuntimeState runtime;
        if (!motion::try_load_runtime(path, runtime)) { runtimeWriteTime = {}; return; }
        if (runtime.activeGroupId == appliedGroupId && runtime.activeMediaId == appliedMediaId &&
            runtime.decodePath == actualDecodePath && runtime.decodeReason == actualDecodeReason) return;
        auto previousGroupId = appliedGroupId;
        appliedGroupId = std::move(runtime.activeGroupId);
        appliedMediaId = std::move(runtime.activeMediaId);
        actualDecodePath = std::move(runtime.decodePath);
        actualDecodeReason = std::move(runtime.decodeReason);
        auto activeGroupId = ActiveGroupId();
        if (activeGroupId == previousGroupId || activeGroupId == appliedGroupId) SyncMediaSelectionToApplied();
        UpdateStatusSummary();
    }

    void MainWindow::ApplySettingsToControls()
    {
        DesktopPlayback().IsOn(settings.desktopPlayback);
        ActivePlayback().IsOn(settings.activePlaybackEnabled);
        select_tag(CoveredBehavior(), settings.continueWhenCovered ? L"continue" : L"pause");
        ScreensaverEnabled().IsOn(settings.screensaverEnabled);
        StartWithWindows().IsOn(settings.startWithWindows);
        select_tag(IdleTimeout(), std::to_wstring(settings.idleTimeoutSeconds));
        select_tag(AutoLockTimeout(), settings.autoLockEnabled ? std::to_wstring(settings.autoLockTimeoutSeconds) : L"0");
        select_tag(DisplayOffAfterLockDelay(), settings.displayOffAfterLockEnabled
            ? std::to_wstring(settings.displayOffAfterLockDelaySeconds) : L"-1");
        select_tag(DecodeMode(), motion::utf8_to_wide(settings.decodeMode));
        select_tag(PerformanceMode(), motion::utf8_to_wide(settings.performanceMode));
        select_tag(DisplayMode(), motion::utf8_to_wide(settings.displayMode));
        bool randomEnabled = !settings.randomGroupId.empty() && settings.randomGroupId == settings.selectedGroupId;
        select_tag(RandomInterval(), randomEnabled ? std::to_wstring(settings.randomIntervalMinutes) : L"-1");
        UpdateStatusSummary();
    }

    void MainWindow::LoadDisplayTargets()
    {
        bool wasInitializing = initializing;
        initializing = true;
        displays = motion::enumerate_displays();
        DisplayTargetPicker().Items().Clear();

        ComboBoxItem all;
        all.Content(box_value(L"所有显示器（相同壁纸）"));
        all.Tag(box_value(L""));
        DisplayTargetPicker().Items().Append(all);

        int selectedIndex = 0;
        bool selectedExists = selectedDisplayId.empty();
        for (size_t index = 0; index < displays.size(); ++index) {
            auto const& display = displays[index];
            auto width = display.bounds.right - display.bounds.left;
            auto height = display.bounds.bottom - display.bounds.top;
            std::wstring label = L"显示器 " + std::to_wstring(index + 1);
            if (display.primary) label += L"（主显示器）";
            label += L" · " + std::to_wstring(width) + L"×" + std::to_wstring(height);
            ComboBoxItem item;
            item.Content(box_value(label));
            item.Tag(box_value(motion::utf8_to_wide(display.id)));
            DisplayTargetPicker().Items().Append(item);
            if (display.id == selectedDisplayId) {
                selectedIndex = static_cast<int>(index + 1);
                selectedExists = true;
            }
        }
        if (!selectedExists) selectedDisplayId.clear();
        DisplayTargetPicker().SelectedIndex(selectedIndex);
        DisplayTargetPicker().IsEnabled(settings.displayMode == "independent" && displays.size() > 1);
        initializing = wasInitializing;
    }

    std::pair<std::string, std::string> MainWindow::SelectedWallpaperForTarget() const
    {
        if (!selectedDisplayId.empty()) {
            auto assignment = std::find_if(settings.displayAssignments.begin(), settings.displayAssignments.end(),
                [&](auto const& value) { return value.displayId == selectedDisplayId; });
            if (assignment != settings.displayAssignments.end()) return { assignment->groupId, assignment->mediaId };
            return { settings.selectedGroupId, settings.selectedMediaId };
        }
        return { appliedGroupId, appliedMediaId };
    }

    void MainWindow::SelectWallpaperForTarget(std::string const& groupId, std::string const& mediaId, std::string const& displayId)
    {
        if (displayId.empty()) {
            settings.selectedGroupId = groupId;
            settings.selectedMediaId = mediaId;
            settings.displayAssignments.clear();
            return;
        }
        auto assignment = std::find_if(settings.displayAssignments.begin(), settings.displayAssignments.end(),
            [&](auto const& value) { return value.displayId == displayId; });
        if (assignment == settings.displayAssignments.end()) {
            settings.displayAssignments.push_back({ displayId, groupId, mediaId });
        } else {
            assignment->groupId = groupId;
            assignment->mediaId = mediaId;
        }
    }

    void MainWindow::RemoveMediaAssignments(std::string const& groupId, std::string const& mediaId)
    {
        std::erase_if(settings.displayAssignments, [&](auto const& assignment) {
            return assignment.groupId == groupId && assignment.mediaId == mediaId;
        });
    }

    void MainWindow::UpdateStatusSummary()
    {
        auto writeLease = TryAcquireLibraryWrite(false);
        if (!writeLease) return;
        CurrentWallpaperName().Text(L"尚未选择壁纸");
        CurrentWallpaperDetails().Text(L"从壁纸分组中选择即可应用");
        CurrentWallpaperPreview().Source(nullptr);
        CurrentWallpaperThumb1().Source(nullptr);
        CurrentWallpaperThumb2().Source(nullptr);
        CurrentWallpaperThumb3().Source(nullptr);
        CurrentWallpaperThumb4().Source(nullptr);
        CurrentWallpaperOverflowOverlay().Visibility(Visibility::Collapsed);
        bool currentIsVideo = false;

        if (motion::valid_id(appliedGroupId) && motion::valid_id(appliedMediaId)) {
            auto media = mediaLibrary->LoadMedia(appliedGroupId);
            auto selected = std::find_if(media.begin(), media.end(), [&](auto const& item) { return item.id == appliedMediaId; });
            if (selected != media.end()) {
                currentIsVideo = selected->kind == "video";
                CurrentWallpaperName().Text(selected->name);
                bool sourceAvailable = mediaLibrary->SourceAvailable(*selected);
                CurrentWallpaperDetails().Text(selected->kind == "image"
                    ? L"静态图片 · " + format_size(selected->sizeBytes)
                    : sourceAvailable ? L"视频 · " + format_size(selected->sizeBytes)
                    : L"视频 · 仅保留性能副本");
                auto cover = mediaLibrary->MediaDirectory(*selected) / selected->coverFileName;
                if (!selected->coverFileName.empty() && fs::is_regular_file(cover)) {
                    CurrentWallpaperPreview().Source(Microsoft::UI::Xaml::Media::Imaging::BitmapImage{ Windows::Foundation::Uri(file_uri(cover)) });
                }
            }

            uint32_t thumbnailIndex = 0;
            for (auto const& item : media) {
                if (thumbnailIndex >= 4) break;
                auto cover = mediaLibrary->MediaDirectory(item) / item.coverFileName;
                if (item.coverFileName.empty() || !fs::is_regular_file(cover)) continue;
                auto source = Microsoft::UI::Xaml::Media::Imaging::BitmapImage{ Windows::Foundation::Uri(file_uri(cover)) };
                switch (thumbnailIndex++) {
                case 0: CurrentWallpaperThumb1().Source(source); break;
                case 1: CurrentWallpaperThumb2().Source(source); break;
                case 2: CurrentWallpaperThumb3().Source(source); break;
                case 3: CurrentWallpaperThumb4().Source(source); break;
                }
            }
            if (media.size() > 4) {
                CurrentWallpaperOverflow().Text(L"+" + std::to_wstring(media.size() - 3));
                CurrentWallpaperOverflowOverlay().Visibility(Visibility::Visible);
            }
        }

        PlaybackStatusText().Text(!settings.desktopPlayback
            ? L"已停止"
            : settings.activePlaybackEnabled ? L"正在播放" : L"已冻结省电");
        std::wstring decodeStatus;
        if (actualDecodePath == "automatic") decodeStatus = L"自动解码 · 已启用 DXGI/DXVA 路径";
        else if (actualDecodePath == "hardware") decodeStatus = L"硬件解码 · 已请求 DXGI/DXVA 路径";
        else if (actualDecodePath == "software-fallback") decodeStatus =
            actualDecodeReason == "no-physical-d3d11-adapter"
            ? L"软件渲染 · 自动回退（无可用物理 D3D11 设备）"
            : actualDecodeReason == "no-d3d11-video-support"
            ? L"CPU 解码 · 物理 GPU 合成"
            : L"软件解码 · 自动回退";
        else if (actualDecodePath == "software") decodeStatus = L"软件解码 · 手动选择";
        else if (actualDecodePath == "unavailable") decodeStatus =
            actualDecodeReason == "no-d3d11-video-device"
            ? L"硬件解码不可用 · 无可用 D3D11 视频设备"
            : L"硬件解码不可用";
        else if (actualDecodePath == "probing") decodeStatus = L"正在检测视频解码器…";
        else decodeStatus = settings.decodeMode == "software"
            ? L"软件解码"
            : settings.decodeMode == "hardware" ? L"硬件解码 · 等待检测" : L"自动解码 · 等待检测";
        DecodeStatusText().Text(decodeStatus);
        DecodeStatusText().Visibility(currentIsVideo || actualDecodePath == "unavailable"
            ? Visibility::Visible : Visibility::Collapsed);
        UpdatePerformanceModeAvailability();
    }

    void MainWindow::UpdatePerformanceModeAvailability()
    {
        auto writeLease = TryAcquireLibraryWrite(false);
        if (!writeLease) return;
        bool originalAvailable = true;
        std::vector<std::pair<std::string, std::string>> selectedMedia;
        if (motion::valid_id(settings.selectedGroupId) && motion::valid_id(settings.selectedMediaId)) {
            selectedMedia.emplace_back(settings.selectedGroupId, settings.selectedMediaId);
        }
        if (settings.displayMode == "independent") {
            for (auto const& assignment : settings.displayAssignments) {
                auto value = std::pair{ assignment.groupId, assignment.mediaId };
                if (std::find(selectedMedia.begin(), selectedMedia.end(), value) == selectedMedia.end()) {
                    selectedMedia.push_back(std::move(value));
                }
            }
        }
        for (auto const& [groupId, mediaId] : selectedMedia) {
            auto media = mediaLibrary->LoadMedia(groupId);
            auto found = std::find_if(media.begin(), media.end(),
                [&](auto const& item) { return item.id == mediaId; });
            if (found != media.end() && found->kind == "video" && !mediaLibrary->SourceAvailable(*found)) {
                originalAvailable = false;
                break;
            }
        }
        for (uint32_t index = 0; index < PerformanceMode().Items().Size(); ++index) {
            auto item = PerformanceMode().Items().GetAt(index).try_as<ComboBoxItem>();
            if (item && unbox_value_or<hstring>(item.Tag(), {}) == L"original") {
                item.IsEnabled(originalAvailable);
                break;
            }
        }
    }

    void MainWindow::LoadGroups()
    {
        auto writeLease = TryAcquireLibraryWrite(false);
        if (!writeLease) return;
        bool wasInitializing = initializing;
        initializing = true;
        GroupPicker().Items().Clear();
        auto loaded = mediaLibrary->LoadGroups();
        groups = std::move(loaded.groups);
        bool settingsChanged = false;

        int selected = 0;
        auto uiGroupId = browsingGroupId.empty() ? settings.selectedGroupId : browsingGroupId;
        bool appliedGroupExists = false;
        for (size_t index = 0; index < groups.size(); ++index) {
            GroupPicker().Items().Append(box_value(groups[index].name));
            if (groups[index].id == uiGroupId) selected = static_cast<int>(index);
            if (groups[index].id == settings.selectedGroupId) appliedGroupExists = true;
        }
        browsingGroupId = groups[static_cast<size_t>(selected)].id;
        if (!appliedGroupExists) {
            settings.selectedGroupId = groups[static_cast<size_t>(selected)].id;
            settings.selectedMediaId.clear();
            settingsChanged = true;
        }
        GroupPicker().SelectedIndex(selected);
        initializing = wasInitializing;
        if (settingsChanged) TrySaveSettings();
    }

    std::string MainWindow::ActiveGroupId()
    {
        auto index = GroupPicker().SelectedIndex();
        if (index >= 0 && static_cast<size_t>(index) < groups.size()) return groups[static_cast<size_t>(index)].id;
        for (auto const& group : groups) if (group.id == settings.selectedGroupId) return group.id;
        return {};
    }

    void MainWindow::LoadMedia()
    {
        auto writeLease = TryAcquireLibraryWrite(false);
        if (!writeLease) return;
        auto groupId = ActiveGroupId();
        allMedia = groupId.empty() ? std::vector<motion::MediaMetadata>{} : mediaLibrary->LoadMedia(groupId);
        RefreshMedia();
        if (!groupId.empty()) RefreshMissingCovers(groupId, allMedia);
    }

    winrt::fire_and_forget MainWindow::RefreshMissingCovers(std::string groupId, std::vector<motion::MediaMetadata> media)
    {
        auto writeLease = TryAcquireLibraryWrite(false);
        if (!writeLease) co_return;
        if (coversRefreshing.exchange(true, std::memory_order_acq_rel)) co_return;
        auto library = mediaLibrary;
        auto weak = get_weak();
        auto dispatcher = DispatcherQueue();
        bool changed{};
        co_await winrt::resume_background();
        try {
            if (!writeLease->RevalidateMediaLibraryTrust()) {
                throw std::runtime_error("media library identity changed before cover refresh");
            }
            for (auto const& item : media) {
                if (!writeLease->RevalidateMediaLibraryTrust()) {
                    throw std::runtime_error("media library identity changed during cover refresh");
                }
                changed = library->EnsureCover(item) || changed;
            }
        } catch (...) {}
        dispatcher.TryEnqueue([weak, groupId = std::move(groupId), changed] {
            if (auto self = weak.get()) {
                self->coversRefreshing.store(false, std::memory_order_release);
                if (changed && self->ActiveGroupId() == groupId) {
                    auto writeLease = self->TryAcquireLibraryWrite(false);
                    if (!writeLease) return;
                    self->allMedia = self->mediaLibrary->LoadMedia(groupId);
                    self->RefreshMedia();
                    self->UpdateStatusSummary();
                } else if (self->ActiveGroupId() != groupId) {
                    self->LoadMedia();
                }
            }
        });
    }

    void MainWindow::RefreshMedia()
    {
        auto writeLease = TryAcquireLibraryWrite(false);
        if (!writeLease) return;
        bool wasInitializing = initializing;
        initializing = true;
        filteredMedia = allMedia;
        auto sort = combo_string(SortPicker(), "name");
        std::stable_sort(filteredMedia.begin(), filteredMedia.end(), [&](auto const& left, auto const& right) {
            if (sort == "newest") return left.importedAt > right.importedAt;
            if (sort == "size") return left.sizeBytes > right.sizeBytes;
            if (sort == "kind" && left.kind != right.kind) return left.kind == "video";
            return _wcsicmp(left.name.c_str(), right.name.c_str()) < 0;
        });
        MediaList().Items().Clear();
        int selected = -1;
        auto selectedWallpaper = SelectedWallpaperForTarget();
        for (size_t index = 0; index < filteredMedia.size(); ++index) {
            auto const& media = filteredMedia[index];
            bool isSelected = media.id == selectedWallpaper.second && media.groupId == selectedWallpaper.first;
            GridViewItem card;
            card.Padding(ThicknessHelper::FromUniformLength(3));
            StackPanel content;
            content.Width(246);
            content.Spacing(8);
            Border preview;
            preview.Width(246);
            preview.Height(148);
            preview.CornerRadius(CornerRadiusHelper::FromUniformRadius(10));
            Grid previewContent;
            auto coverPath = mediaLibrary->MediaDirectory(media) / media.coverFileName;
            if (!media.coverFileName.empty() && fs::is_regular_file(coverPath)) {
                Image image;
                image.Stretch(Microsoft::UI::Xaml::Media::Stretch::UniformToFill);
                image.Source(Microsoft::UI::Xaml::Media::Imaging::BitmapImage{ Windows::Foundation::Uri(file_uri(coverPath)) });
                previewContent.Children().Append(image);
            } else {
                FontIcon icon;
                icon.Glyph(media.kind == "image" ? L"\xEB9F" : L"\xE714");
                icon.FontSize(32);
                icon.Opacity(0.55);
                previewContent.Children().Append(icon);
            }
            preview.Child(previewContent);
            TextBlock name;
            name.Text(media.name);
            name.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
            name.TextTrimming(TextTrimming::CharacterEllipsis);
            content.Children().Append(preview);
            content.Children().Append(name);
            card.Content(content);
            set_media_card_selected(card, isSelected);
            MediaList().Items().Append(card);
            if (isSelected) selected = static_cast<int>(index);
        }
        MediaCount().Text(std::to_wstring(filteredMedia.size()) + L" 个壁纸");
        MediaList().SelectedIndex(selected);
        UpdateMediaActionState();
        initializing = wasInitializing;
    }

    void MainWindow::UpdateMediaSelectionVisuals(int32_t selectedIndex)
    {
        auto count = MediaList().Items().Size();
        for (uint32_t index = 0; index < count; ++index) {
            auto card = MediaList().Items().GetAt(index).try_as<GridViewItem>();
            if (card) set_media_card_selected(card, static_cast<int32_t>(index) == selectedIndex);
        }
    }

    void MainWindow::SyncMediaSelectionToApplied()
    {
        auto selectedWallpaper = SelectedWallpaperForTarget();
        int32_t selectedIndex = -1;
        for (size_t index = 0; index < filteredMedia.size(); ++index) {
            auto const& media = filteredMedia[index];
            if (media.groupId == selectedWallpaper.first && media.id == selectedWallpaper.second) {
                selectedIndex = static_cast<int32_t>(index);
                break;
            }
        }
        bool wasInitializing = initializing;
        initializing = true;
        MediaList().SelectedIndex(selectedIndex);
        initializing = wasInitializing;
        UpdateMediaSelectionVisuals(selectedIndex);
        UpdateMediaActionState();
    }

    void MainWindow::UpdateMediaActionState()
    {
        auto index = MediaList().SelectedIndex();
        bool valid = index >= 0 && static_cast<size_t>(index) < filteredMedia.size();
        DeleteMediaButton().IsEnabled(valid);
        RenameMediaButton().IsEnabled(valid);
        MoveMediaButton().IsEnabled(valid && groups.size() > 1);

    }

    void MainWindow::Sort_Changed(IInspectable const&, SelectionChangedEventArgs const&) { if (!initializing) RefreshMedia(); }

    void MainWindow::Settings_Changed(IInspectable const&, RoutedEventArgs const&)
    {
        if (initializing) return;
        settings.desktopPlayback = DesktopPlayback().IsOn();
        settings.activePlaybackEnabled = ActivePlayback().IsOn();
        settings.screensaverEnabled = ScreensaverEnabled().IsOn();
        settings.startWithWindows = StartWithWindows().IsOn();
        UpdateStatusSummary();
        settingsSaveTimer.Stop();
        settingsSaveTimer.Start();
    }

    void MainWindow::Policy_Changed(IInspectable const&, SelectionChangedEventArgs const&)
    {
        if (initializing) return;
        settings.idleTimeoutSeconds = combo_int(IdleTimeout(), 30);
        int autoLock = combo_int(AutoLockTimeout(), 300);
        settings.autoLockEnabled = autoLock > 0;
        settings.autoLockTimeoutSeconds = autoLock > 0 ? autoLock : 300;
        int displayOffAfterLock = combo_int(DisplayOffAfterLockDelay(), 30);
        settings.displayOffAfterLockEnabled = displayOffAfterLock >= 0;
        settings.displayOffAfterLockDelaySeconds = displayOffAfterLock >= 0 ? displayOffAfterLock : 30;
        settings.continueWhenCovered = combo_string(CoveredBehavior(), "pause") == "continue";
        settings.decodeMode = combo_string(DecodeMode(), "auto");
        settings.performanceMode = combo_string(PerformanceMode(), "balanced");
        auto previousDisplayMode = settings.displayMode;
        settings.displayMode = combo_string(DisplayMode(), "independent");
        if (settings.displayMode != previousDisplayMode) LoadDisplayTargets();
        UpdateStatusSummary();
        TrySaveSettings();
    }

    void MainWindow::SystemSettings_Click(IInspectable const&, RoutedEventArgs const&) { ShowSettingsPage(); }
    void MainWindow::Variants_Click(IInspectable const&, RoutedEventArgs const&) { ShowVariantsPage(); }

    void MainWindow::CurrentWallpaper_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (!motion::valid_id(settings.selectedGroupId)) return;
        for (size_t index = 0; index < groups.size(); ++index) {
            if (groups[index].id != settings.selectedGroupId) continue;
            bool wasInitializing = initializing;
            initializing = true;
            GroupPicker().SelectedIndex(static_cast<int32_t>(index));
            browsingGroupId = settings.selectedGroupId;
            initializing = wasInitializing;
            LoadMedia();
            ShowWallpaperPage();
            return;
        }
    }

    void MainWindow::OpenNewGroup_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (libraryAccessGate->MigrationInProgress()) {
            ShowStatus(L"媒体库正在迁移，请稍后再创建分组。", true);
            return;
        }
        if (auto flyout = AddGroupButton().Flyout()) flyout.ShowAt(AddGroupButton());
    }

    void MainWindow::ShowSettingsPage()
    {
        Navigate(AppPage::Settings);
    }

    void MainWindow::ShowVariantsPage()
    {
        RefreshVariants();
        Navigate(AppPage::Variants);
    }

    void MainWindow::Navigate(AppPage page)
    {
        currentPage = page;
        SettingsPage().Visibility(page == AppPage::Settings ? Visibility::Visible : Visibility::Collapsed);
        VariantsPage().Visibility(page == AppPage::Variants ? Visibility::Visible : Visibility::Collapsed);
        WallpaperPage().Visibility(page == AppPage::WallpaperGroup ? Visibility::Visible : Visibility::Collapsed);
        SettingsNavIndicator().Visibility(page == AppPage::Settings ? Visibility::Visible : Visibility::Collapsed);
        VariantsNavIndicator().Visibility(page == AppPage::Variants ? Visibility::Visible : Visibility::Collapsed);
        if (page == AppPage::WallpaperGroup) return;
        bool wasInitializing = initializing;
        initializing = true;
        GroupPicker().SelectedIndex(-1);
        initializing = wasInitializing;
    }

    void MainWindow::ShowWallpaperPage()
    {
        auto index = GroupPicker().SelectedIndex();
        if (index < 0 || static_cast<size_t>(index) >= groups.size()) return;
        GroupTitle().Text(groups[static_cast<size_t>(index)].name);
        LoadDisplayTargets();
        RefreshMedia();
        Navigate(AppPage::WallpaperGroup);
    }

    void MainWindow::RefreshVariants()
    {
        auto writeLease = TryAcquireLibraryWrite(false);
        if (!writeLease) return;
        auto items = motion::app::load_variant_page(*mediaLibrary, groups);
        std::stable_sort(items.begin(), items.end(), [](auto const& left, auto const& right) {
            return _wcsicmp(left.media.name.c_str(), right.media.name.c_str()) < 0;
        });

        uint64_t totalBytes{};
        uint32_t totalFiles{};
        uint32_t taskCount{};
        optimizationWorkVisible = false;
        for (auto const& item : items) {
            totalBytes += item.status.bytes;
            totalFiles += item.status.files;
            optimizationWorkVisible = optimizationWorkVisible || item.status.queued || item.status.generating;
            if (item.status.queued) ++taskCount;
        }

        SYSTEM_POWER_STATUS powerStatus{};
        bool waitingForPower = GetSystemPowerStatus(&powerStatus) && powerStatus.ACLineStatus == 0;
        auto fingerprint = variant_page_structure_fingerprint(items, waitingForPower);
        if (fingerprint == variantViewFingerprint) {
            bool controlsMatch = variantTaskCards.size() == static_cast<size_t>(taskCount) &&
                VariantTasks().Children().Size() == taskCount;
            if (controlsMatch) {
                for (auto const& item : items) {
                    if (!item.status.queued) continue;
                    auto found = variantTaskCards.find(item.media.id);
                    if (found == variantTaskCards.end() || !found->second.root ||
                        !found->second.stateText || !found->second.progress ||
                        !found->second.pause || !found->second.cancel) {
                        controlsMatch = false;
                        break;
                    }
                }
            }
            if (controlsMatch) {
                for (auto const& item : items) {
                    if (!item.status.queued) continue;
                    motion::app::update_variant_task_card(
                        variantTaskCards.at(item.media.id), item, waitingForPower);
                }
                return;
            }
        }

        variantTaskCards.clear();
        VariantTasks().Children().Clear();
        VariantCards().Children().Clear();
        VariantSummaryText().Text(std::to_wstring(items.size()) + L" 个视频 · " +
            std::to_wstring(totalFiles) + L" 个副本 · " + format_size(totalBytes));
        VariantEmptyState().Visibility(items.empty() ? Visibility::Visible : Visibility::Collapsed);

        auto stroke = Microsoft::UI::Xaml::Media::SolidColorBrush{
            Windows::UI::ColorHelper::FromArgb(255, 220, 226, 232) };
        auto surface = Microsoft::UI::Xaml::Media::SolidColorBrush{ Windows::UI::Colors::White() };
        auto muted = Microsoft::UI::Xaml::Media::SolidColorBrush{
            Windows::UI::ColorHelper::FromArgb(255, 245, 247, 249) };

        VariantTasksSection().Visibility(taskCount ? Visibility::Visible : Visibility::Collapsed);
        VariantTaskSummaryText().Text(taskCount ? std::to_wstring(taskCount) + L" 个任务" : L"");
        for (auto const& item : items) {
            if (!item.status.queued) continue;
            auto cover = mediaLibrary->MediaDirectory(item.media) / item.media.coverFileName;
            auto taskCard = motion::app::create_variant_task_card_view(
                item, cover, waitingForPower,
                [weak = get_weak(), media = item.media](bool paused) {
                    if (auto self = weak.get()) self->SetVariantPaused(media, paused);
                },
                [weak = get_weak(), media = item.media] {
                    if (auto self = weak.get()) self->CancelVariant(media);
                });
            VariantTasks().Children().Append(taskCard.root);
            variantTaskCards.insert_or_assign(item.media.id, std::move(taskCard));
        }

        for (auto const& item : items) {
            Border card;
            card.Padding(ThicknessHelper::FromUniformLength(16));
            card.CornerRadius(CornerRadiusHelper::FromUniformRadius(12));
            card.BorderThickness(ThicknessHelper::FromUniformLength(1));
            card.BorderBrush(stroke);
            card.Background(surface);

            Grid layout;
            layout.ColumnSpacing(22);
            layout.ColumnDefinitions().Append(ColumnDefinition{});
            layout.ColumnDefinitions().GetAt(0).Width(GridLengthHelper::FromPixels(250));
            for (int column = 0; column < 3; ++column) {
                ColumnDefinition definition;
                definition.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
                layout.ColumnDefinitions().Append(definition);
            }
            ColumnDefinition actions;
            actions.Width(GridLengthHelper::Auto());
            layout.ColumnDefinitions().Append(actions);

            Grid identity;
            identity.ColumnSpacing(12);
            ColumnDefinition previewColumn;
            previewColumn.Width(GridLengthHelper::FromPixels(92));
            identity.ColumnDefinitions().Append(previewColumn);
            ColumnDefinition identityTextColumn;
            identityTextColumn.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
            identity.ColumnDefinitions().Append(identityTextColumn);
            Border preview;
            preview.Width(92);
            preview.Height(58);
            preview.CornerRadius(CornerRadiusHelper::FromUniformRadius(8));
            preview.Background(muted);
            auto cover = mediaLibrary->MediaDirectory(item.media) / item.media.coverFileName;
            if (!item.media.coverFileName.empty() && fs::is_regular_file(cover)) {
                Image image;
                image.Stretch(Microsoft::UI::Xaml::Media::Stretch::UniformToFill);
                image.Source(Microsoft::UI::Xaml::Media::Imaging::BitmapImage{
                    Windows::Foundation::Uri(file_uri(cover)) });
                preview.Child(image);
            }
            identity.Children().Append(preview);
            StackPanel identityText;
            identityText.VerticalAlignment(VerticalAlignment::Center);
            identityText.Spacing(4);
            TextBlock name;
            name.Text(item.media.name);
            name.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
            name.TextTrimming(TextTrimming::CharacterEllipsis);
            TextBlock group;
            group.Text(item.groupName);
            group.FontSize(12);
            group.Opacity(0.58);
            identityText.Children().Append(name);
            identityText.Children().Append(group);
            Grid::SetColumn(identityText, 1);
            identity.Children().Append(identityText);
            layout.Children().Append(identity);

            bool balancedActive = item.status.requestedMode == "balanced" &&
                (item.status.queued || item.status.generating);
            bool powerSaverActive = item.status.requestedMode == "power-saver" &&
                (item.status.queued || item.status.generating);
            uint8_t availableSelections = item.sourceAvailable ? variant_source : 0;
            if (item.balanced.files || balancedActive) availableSelections |= variant_balanced;
            if (item.powerSaver.files || powerSaverActive) availableSelections |= variant_power_saver;

            uint8_t selection{};
            if (auto selected = variantSelections.find(item.media.id); selected != variantSelections.end()) {
                selection = selected->second & availableSelections;
                if (selection) selected->second = selection;
                else variantSelections.erase(selected);
            }

            auto selectionControls = std::make_shared<VariantSelectionControls>();
            selectionControls->mediaName = item.media.name;
            selectionControls->available = availableSelections;

            Button remove;
            remove.Foreground(Microsoft::UI::Xaml::Media::SolidColorBrush{
                Windows::UI::ColorHelper::FromArgb(255, 196, 43, 28) });
            remove.VerticalAlignment(VerticalAlignment::Center);
            remove.Click([weak = get_weak(), media = item.media](auto const&, auto const&) {
                if (auto self = weak.get()) {
                    auto selection = self->variantSelections.find(media.id);
                    if (selection != self->variantSelections.end()) {
                        self->ConfirmDeleteVariantSelection(media, selection->second);
                    }
                }
            });
            selectionControls->remove = winrt::weak_ref<Button>{ remove };
            Grid::SetColumn(remove, 4);
            layout.Children().Append(remove);

            auto selectProfile = [weak = get_weak(), mediaId = item.media.id,
                availableSelections, selectionControls](uint8_t profile, bool checked) {
                if (auto self = weak.get()) {
                    uint8_t current{};
                    if (auto selected = self->variantSelections.find(mediaId);
                        selected != self->variantSelections.end()) current = selected->second;
                    if (profile == variant_source) {
                        if (checked) current = variant_source;
                        else current = 0;
                    } else if (checked) {
                        current &= static_cast<uint8_t>(~variant_source);
                        current |= profile;
                    } else {
                        current &= static_cast<uint8_t>(~(profile | variant_source));
                    }
                    current &= availableSelections;
                    if (current) self->variantSelections[mediaId] = current;
                    else self->variantSelections.erase(mediaId);
                    update_variant_selection_controls(selectionControls, current);
                }
            };

            auto appendProfile = [&](int column, std::wstring const& title,
                motion::app::VariantProfileSummary const& profile, std::string const& mode) {
                StackPanel panel;
                panel.Spacing(5);
                CheckBox heading;
                heading.Content(box_value(title));
                auto profileContext = item.media.name + L"，" + title + L"性能副本";
                Automation::AutomationProperties::SetName(
                    heading, hstring(profileContext + L"，保留选择"));
                heading.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
                TextBlock detail;
                bool active = item.status.requestedMode == mode && (item.status.queued || item.status.generating);
                bool suppressed = mode == "balanced"
                    ? item.status.balancedSuppressed : item.status.powerSaverSuppressed;
                bool profileFailed = item.status.failed && item.status.failedMode == mode;
                if (active) detail.Text(item.status.paused
                    ? L"已暂停"
                    : item.status.generating ? L"正在生成"
                    : waitingForPower ? L"等待接通电源" : L"等待生成");
                else if (profile.files) {
                    auto value = std::to_wstring(profile.files) + L" 个 · " + format_size(profile.bytes);
                    if (profile.sharedStorage) value += L" · 共享存储";
                    if (profileFailed) value += L" · 新规格生成失败";
                    detail.Text(value);
                } else if (!item.sourceAvailable) detail.Text(L"源文件已删除 · 无法生成");
                else if (profileFailed) detail.Text(L"生成失败 · 可重试");
                else detail.Text(suppressed ? L"已删除 · 不会自动生成" : L"未生成");
                detail.FontSize(12);
                detail.Opacity(0.62);
                heading.IsEnabled(profile.files || active);
                auto profileSelection = mode == "balanced" ? variant_balanced : variant_power_saver;
                if ((selection & profileSelection) && heading.IsEnabled()) {
                    heading.IsChecked(box_value(true).as<Windows::Foundation::IReference<bool>>());
                }
                if (profileSelection == variant_balanced) {
                    selectionControls->balanced = winrt::weak_ref<CheckBox>{ heading };
                } else {
                    selectionControls->powerSaver = winrt::weak_ref<CheckBox>{ heading };
                }
                heading.Click([selectProfile, profileSelection](IInspectable const& sender, auto const&) {
                    auto checked = sender.as<CheckBox>().IsChecked();
                    selectProfile(profileSelection, checked && checked.Value());
                });
                Button action;
                auto actionLabel = active ? std::wstring(L"任务进行中")
                    : profileFailed ? std::wstring(L"重试")
                    : profile.files ? std::wstring(L"已生成") : std::wstring(L"生成");
                action.Content(box_value(actionLabel));
                Automation::AutomationProperties::SetName(
                    action, hstring(profileContext + L"，" + actionLabel));
                action.HorizontalAlignment(HorizontalAlignment::Left);
                action.Padding(ThicknessHelper::FromLengths(12, 5, 12, 5));
                if (active) {
                    action.IsEnabled(false);
                } else if (profileFailed && item.sourceAvailable) {
                    action.Click([weak = get_weak(), media = item.media, mode](auto const&, auto const&) {
                        if (auto self = weak.get()) self->RequestVariant(media, mode);
                    });
                } else if (!profile.files && item.sourceAvailable) {
                    action.Click([weak = get_weak(), media = item.media, mode](auto const&, auto const&) {
                        if (auto self = weak.get()) self->RequestVariant(media, mode);
                    });
                } else if (!profile.files) {
                    action.Content(box_value(L"需要源文件"));
                    Automation::AutomationProperties::SetName(
                        action, hstring(profileContext + L"，需要源文件"));
                    action.IsEnabled(false);
                } else {
                    action.IsEnabled(false);
                }
                panel.Children().Append(heading);
                panel.Children().Append(detail);
                panel.Children().Append(action);
                Grid::SetColumn(panel, column);
                layout.Children().Append(panel);
            };

            StackPanel source;
            source.Spacing(5);
            CheckBox sourceTitle;
            sourceTitle.Content(box_value(L"源文件"));
            Automation::AutomationProperties::SetName(
                sourceTitle, hstring(item.media.name + L"，源文件，保留选择"));
            sourceTitle.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
            sourceTitle.IsEnabled(item.sourceAvailable);
            if (selection & variant_source) {
                sourceTitle.IsChecked(box_value(true).as<Windows::Foundation::IReference<bool>>());
            }
            selectionControls->source = winrt::weak_ref<CheckBox>{ sourceTitle };
            sourceTitle.Click([selectProfile](IInspectable const& sender, auto const&) {
                auto checked = sender.as<CheckBox>().IsChecked();
                selectProfile(variant_source, checked && checked.Value());
            });
            TextBlock sourceDetail;
            sourceDetail.Text(item.sourceAvailable
                ? format_size(item.media.sizeBytes)
                : L"已删除 · 已释放 " + format_size(item.media.sizeBytes));
            sourceDetail.FontSize(12);
            sourceDetail.Opacity(0.62);
            TextBlock sourceState;
            sourceState.Text(item.sourceAvailable
                ? L"可单独移入回收站"
                : L"无法选择原画或重新生成");
            sourceState.FontSize(12);
            sourceState.Opacity(0.62);
            source.Children().Append(sourceTitle);
            source.Children().Append(sourceDetail);
            source.Children().Append(sourceState);
            Grid::SetColumn(source, 1);
            layout.Children().Append(source);

            appendProfile(2, L"自动平衡", item.balanced, "balanced");
            appendProfile(3, L"低功耗", item.powerSaver, "power-saver");
            update_variant_selection_controls(selectionControls, selection);

            card.Child(layout);
            VariantCards().Children().Append(card);
        }
        variantViewFingerprint = std::move(fingerprint);
    }

    void MainWindow::GroupPicker_SelectionChanged(IInspectable const&, SelectionChangedEventArgs const&)
    {
        if (initializing || reorderingGroups || GroupPicker().SelectedIndex() < 0) return;
        browsingGroupId = ActiveGroupId();
        LoadMedia();
        ShowWallpaperPage();
    }

    void MainWindow::GroupPicker_RightTapped(IInspectable const&,
        Microsoft::UI::Xaml::Input::RightTappedRoutedEventArgs const& args)
    {
        auto source = args.OriginalSource().try_as<DependencyObject>();
        while (source && !source.try_as<ListViewItem>()) {
            source = Microsoft::UI::Xaml::Media::VisualTreeHelper::GetParent(source);
        }
        auto item = source.try_as<ListViewItem>();
        if (!item) return;
        auto index = GroupPicker().IndexFromContainer(item);
        if (index < 0 || static_cast<size_t>(index) >= groups.size()) return;

        bool wasInitializing = initializing;
        initializing = true;
        GroupPicker().SelectedIndex(index);
        browsingGroupId = groups[static_cast<size_t>(index)].id;
        initializing = wasInitializing;
        args.Handled(true);
        RenameActiveGroup();
    }

    void MainWindow::GroupPicker_DragItemsStarting(IInspectable const&, DragItemsStartingEventArgs const& args)
    {
        if (libraryAccessGate->MigrationInProgress()) return;
        if (args.Items().Size() == 0) return;
        auto name = unbox_value_or<hstring>(args.Items().GetAt(0), {});
        auto found = std::find_if(groups.begin(), groups.end(), [&](auto const& group) { return group.name == name; });
        if (found == groups.end()) return;
        draggedGroupId = found->id;
        browsingGroupId = draggedGroupId;
        reorderingGroups = true;
    }

    void MainWindow::GroupPicker_DragItemsCompleted(IInspectable const&, DragItemsCompletedEventArgs const&)
    {
        if (!reorderingGroups) return;
        reorderingGroups = false;
        auto writeLease = TryAcquireLibraryWrite();
        if (!writeLease) {
            draggedGroupId.clear();
            return;
        }
        try {
            std::vector<std::string> orderedIds;
            orderedIds.reserve(GroupPicker().Items().Size());
            for (uint32_t index = 0; index < GroupPicker().Items().Size(); ++index) {
                auto name = unbox_value_or<hstring>(GroupPicker().Items().GetAt(index), {});
                auto found = std::find_if(groups.begin(), groups.end(), [&](auto const& group) { return group.name == name; });
                if (found == groups.end()) throw std::runtime_error("unknown reordered group");
                orderedIds.push_back(found->id);
            }

            if (orderedIds.size() != groups.size()) throw std::runtime_error("incomplete group order");
            bool changed = false;
            for (size_t index = 0; index < orderedIds.size(); ++index) {
                if (orderedIds[index] == groups[index].id) continue;
                changed = true;
                break;
            }
            if (changed) mediaLibrary->SetGroupOrder(orderedIds, groups);
            browsingGroupId = draggedGroupId;
            LoadGroups();
            LoadMedia();
            if (changed) ShowStatus(L"分组顺序已更新。");
        } catch (...) {
            LoadGroups();
            ShowStatus(L"无法保存分组顺序，已恢复原顺序。", true);
        }
        draggedGroupId.clear();
    }

    void MainWindow::Media_SelectionChanged(IInspectable const&, SelectionChangedEventArgs const&)
    {
        if (initializing) return;
        auto writeLease = TryAcquireLibraryWrite();
        if (!writeLease) return;
        auto index = MediaList().SelectedIndex();
        bool valid = index >= 0 && static_cast<size_t>(index) < filteredMedia.size();
        UpdateMediaActionState();
        if (!valid) return;
        UpdateMediaSelectionVisuals(index);
        auto previousSettings = settings;
        auto const& media = filteredMedia[static_cast<size_t>(index)];
        SelectWallpaperForTarget(media.groupId, media.id, selectedDisplayId);
        if (media.kind == "video" && settings.performanceMode == "original" &&
            !mediaLibrary->SourceAvailable(media)) {
            auto status = mediaLibrary->VariantStatus(media);
            auto retained = motion::select_variant_file(status, "original");
            auto entry = std::find_if(status.entries.begin(), status.entries.end(),
                [&](auto const& value) { return value.fileName == retained; });
            settings.performanceMode = entry != status.entries.end() ? entry->mode : "balanced";
            bool wasInitializing = initializing;
            initializing = true;
            select_tag(PerformanceMode(), motion::utf8_to_wide(settings.performanceMode));
            initializing = wasInitializing;
        }
        if (!TrySaveSettings()) {
            settings = std::move(previousSettings);
            RefreshMedia();
            return;
        }
        ShowStatus(selectedDisplayId.empty() ? L"正在应用到所有显示器…" : L"正在应用到所选显示器…");
    }

    void MainWindow::DisplayTargetPicker_Changed(IInspectable const&, SelectionChangedEventArgs const&)
    {
        if (initializing) return;
        auto item = DisplayTargetPicker().SelectedItem().try_as<ComboBoxItem>();
        selectedDisplayId = item ? motion::wide_to_utf8(unbox_value_or<hstring>(item.Tag(), {}).c_str()) : std::string{};
        RefreshMedia();
    }

    void MainWindow::CreateGroup_Click(IInspectable const&, RoutedEventArgs const&)
    {
        auto writeLease = TryAcquireLibraryWrite();
        if (!writeLease) return;
        try {
            auto group = mediaLibrary->CreateGroup(NewGroupName().Text().c_str(), groups);
            NewGroupName().Text(L"");
            settings.selectedGroupId = group.id;
            settings.selectedMediaId.clear();
            browsingGroupId = group.id;
            SaveSettings();
            LoadGroups();
            initializing = true;
            for (size_t index = 0; index < groups.size(); ++index) if (groups[index].id == group.id) GroupPicker().SelectedIndex(static_cast<int32_t>(index));
            initializing = false;
            LoadMedia();
            ShowWallpaperPage();
            ShowStatus(L"已创建壁纸分组");
        } catch (...) {
            ShowStatus(L"无法创建分组，请检查名称是否为空或重复。", true);
        }
    }

    void MainWindow::RenameGroup_Click(IInspectable const&, RoutedEventArgs const&)
    {
        RenameActiveGroup();
    }

    void MainWindow::RenameActiveGroup()
    {
        if (libraryAccessGate->MigrationInProgress()) {
            ShowStatus(L"媒体库正在迁移，请稍后再重命名分组。", true);
            return;
        }
        auto selected = GroupPicker().SelectedIndex();
        if (selected < 0 || static_cast<size_t>(selected) >= groups.size()) return;
        auto group = groups[static_cast<size_t>(selected)];
        TextBox input;
        input.Text(group.name);
        input.SelectAll();
        input.MaxLength(80);
        ContentDialog dialog;
        dialog.XamlRoot(Content().as<FrameworkElement>().XamlRoot());
        dialog.Title(box_value(L"重命名分组"));
        dialog.Content(input);
        dialog.PrimaryButtonText(L"保存");
        dialog.CloseButtonText(L"取消");
        dialog.DefaultButton(ContentDialogButton::Primary);
        auto operation = dialog.ShowAsync();
        operation.Completed([weak = get_weak(), group, input](auto const& result, Windows::Foundation::AsyncStatus status) {
            if (status != Windows::Foundation::AsyncStatus::Completed || result.GetResults() != ContentDialogResult::Primary) return;
            if (auto self = weak.get()) {
                auto writeLease = self->TryAcquireLibraryWrite();
                if (!writeLease) return;
                try {
                    self->mediaLibrary->RenameGroup(group, input.Text().c_str(), self->groups);
                    self->LoadGroups();
                    self->ShowWallpaperPage();
                    self->ShowStatus(L"分组名称已更新。");
                } catch (...) { self->ShowStatus(L"重命名失败，请检查名称是否为空或重复。", true); }
            }
        });
    }

    void MainWindow::ReorderActiveGroup(int direction)
    {
        auto writeLease = TryAcquireLibraryWrite();
        if (!writeLease) return;
        auto groupId = ActiveGroupId();
        if (groupId.empty()) return;
        try {
            mediaLibrary->ReorderGroup(groupId, direction, groups);
            LoadGroups();
            initializing = true;
            for (size_t index = 0; index < groups.size(); ++index) if (groups[index].id == groupId) GroupPicker().SelectedIndex(static_cast<int32_t>(index));
            initializing = false;
            LoadMedia();
        } catch (...) { ShowStatus(L"无法调整分组顺序。", true); }
    }

    void MainWindow::MoveGroupUp_Click(IInspectable const&, RoutedEventArgs const&) { ReorderActiveGroup(-1); }
    void MainWindow::MoveGroupDown_Click(IInspectable const&, RoutedEventArgs const&) { ReorderActiveGroup(1); }

    void MainWindow::DeleteGroup_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (libraryAccessGate->MigrationInProgress()) {
            ShowStatus(L"媒体库正在迁移，请稍后再删除分组。", true);
            return;
        }
        auto selected = GroupPicker().SelectedIndex();
        if (selected < 0 || static_cast<size_t>(selected) >= groups.size()) return;
        if (groups.size() <= 1) { ShowStatus(L"至少需要保留一个壁纸分组。", true); return; }
        auto group = groups[static_cast<size_t>(selected)];
        ContentDialog dialog;
        dialog.XamlRoot(Content().as<FrameworkElement>().XamlRoot());
        dialog.Title(box_value(L"删除“" + group.name + L"”？"));
        dialog.Content(box_value(L"分组及其中的壁纸会移入 Windows 回收站，可在回收站中恢复。"));
        dialog.PrimaryButtonText(L"移到回收站");
        dialog.CloseButtonText(L"取消");
        dialog.DefaultButton(ContentDialogButton::Close);
        auto operation = dialog.ShowAsync();
        operation.Completed([weak = get_weak(), group](auto const& result, Windows::Foundation::AsyncStatus status) {
            if (status != Windows::Foundation::AsyncStatus::Completed || result.GetResults() != ContentDialogResult::Primary) return;
            if (auto self = weak.get()) self->DeleteGroup(group);
        });
    }

    winrt::fire_and_forget MainWindow::DeleteGroup(motion::GroupMetadata group)
    {
        if (activeLibraryMigrationPause) {
            ShowStatus(L"媒体库操作正在等待后台服务，请稍后再试。", true);
            co_return;
        }
        auto writeLease = TryAcquireLibraryWrite();
        if (!writeLease) co_return;
        auto weak = get_weak();
        auto library = mediaLibrary;
        auto dispatcher = DispatcherQueue();
        auto previousSettings = settings;
        auto previousBrowsingGroup = browsingGroupId;
        try {
            if (settings.selectedGroupId == group.id) {
                auto replacement = std::find_if(groups.begin(), groups.end(),
                    [&](auto const& item) { return item.id != group.id; });
                if (replacement == groups.end()) {
                    throw std::runtime_error("replacement group not found");
                }
                settings.selectedGroupId = replacement->id;
                settings.selectedMediaId.clear();
            }
            if (browsingGroupId == group.id) browsingGroupId.clear();
            if (settings.randomGroupId == group.id) settings.randomGroupId.clear();
            std::erase_if(settings.displayAssignments,
                [&](auto const& assignment) { return assignment.groupId == group.id; });
            SaveSettings();
        } catch (...) {
            settings = std::move(previousSettings);
            browsingGroupId = std::move(previousBrowsingGroup);
            try { SaveSettings(); } catch (...) {}
            ShowStatus(L"无法安全切换当前分组；未删除任何文件。", true);
            co_return;
        }

        std::shared_ptr<motion::app::AgentLibraryMigrationPause> agentPause;
        try {
            agentPause = std::make_shared<motion::app::AgentLibraryMigrationPause>();
        } catch (...) {
            settings = std::move(previousSettings);
            browsingGroupId = std::move(previousBrowsingGroup);
            ApplySettingsToControls();
            try { SaveSettings(); } catch (...) {}
            ShowStatus(L"无法建立与后台服务的删除协调通道；未删除分组。", true);
            co_return;
        }
        activeLibraryMigrationPause = agentPause;
        SetLibraryMigrationUi(true);
        ShowStatus(L"正在暂停后台播放并安全删除分组…", false);

        co_await winrt::resume_background();
        bool paused = agentPause->RequestAndWait(std::chrono::seconds(20));
        bool deleted{};
        if (paused && !closing.load(std::memory_order_acquire)) {
            try {
                if (!writeLease->RevalidateMediaLibraryTrust()) {
                    throw std::runtime_error("media library identity changed before group deletion");
                }
                library->DeleteGroup(group);
                deleted = true;
            } catch (...) {}
        }
        bool resumed{};
        if (paused) resumed = agentPause->ResumeAndWait(std::chrono::seconds(20));
        else agentPause->Cancel();

        dispatcher.TryEnqueue([weak, agentPause = std::move(agentPause),
            writeLease = std::move(writeLease), previousSettings = std::move(previousSettings),
            previousBrowsingGroup = std::move(previousBrowsingGroup), paused, deleted, resumed]() mutable {
            if (auto self = weak.get()) {
                if (self->activeLibraryMigrationPause == agentPause) self->activeLibraryMigrationPause.reset();
                writeLease.reset();
                self->SetLibraryMigrationUi(false);
                if (!deleted) {
                    self->settings = std::move(previousSettings);
                    self->browsingGroupId = std::move(previousBrowsingGroup);
                    self->ApplySettingsToControls();
                    try { self->SaveSettings(); } catch (...) {}
                }
                self->LoadGroups();
                self->LoadMedia();
                if (!paused) {
                    self->ShowStatus(L"后台服务未确认暂停；为保护正在使用的文件，未删除分组。", true);
                } else if (!deleted) {
                    self->ShowStatus(L"删除已安全中止；媒体库身份可能已变化，未继续按原路径操作。", true);
                } else if (!resumed) {
                    self->ShowStatus(L"分组已删除，但后台服务未及时确认恢复；请重启应用。", true);
                } else {
                    self->ShowStatus(L"分组已移到 Windows 回收站。");
                }
            }
        });
    }

    void MainWindow::ImportVideo_Click(IInspectable const&, RoutedEventArgs const&)
    {
        ImportFiles("video", L"视频文件", L"*.mp4;*.m4v;*.mov;*.mkv;*.webm;*.avi");
    }

    void MainWindow::ImportImage_Click(IInspectable const&, RoutedEventArgs const&)
    {
        ImportFiles("image", L"图片文件", L"*.jpg;*.jpeg;*.png;*.bmp;*.gif;*.tif;*.tiff;*.webp");
    }

    void MainWindow::CancelImport_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (importCancellation) importCancellation->store(true, std::memory_order_release);
        ImportStatusText().Text(L"正在取消，已完成的文件会保留…");
        CancelImportButton().IsEnabled(false);
    }

    winrt::fire_and_forget MainWindow::ImportFiles(std::string kind, std::wstring title, std::wstring pattern)
    {
        auto weak = get_weak();
        auto dispatcher = DispatcherQueue();
        try {
            HWND window{};
            auto native = this->try_as<::IWindowNative>();
            check_hresult(native->get_WindowHandle(&window));
            auto files = select_files(window, title.c_str(), pattern.c_str());
            if (files.empty()) co_return;
            auto writeLease = TryAcquireLibraryWrite();
            if (!writeLease || importing.exchange(true)) co_return;
            auto library = mediaLibrary;
            auto groupId = ActiveGroupId();
            auto displayId = selectedDisplayId;
            auto optimizationMode = settings.performanceMode;
            auto cancellation = std::make_shared<std::atomic_bool>();
            importCancellation = cancellation;
            ImportImageButton().IsEnabled(false);
            ImportVideoButton().IsEnabled(false);
            CancelImportButton().IsEnabled(true);
            ImportPanel().Visibility(Visibility::Visible);
            ImportProgress().Value(0);
            ImportPercentText().Text(L"0%");
            ImportStatusText().Text(files.size() == 1 ? L"正在后台导入 1 个文件…" : L"正在后台导入多个文件…");

            uint64_t totalBytes{};
            for (auto const& path : files) totalBytes += fs::file_size(path);
            co_await winrt::resume_background();

            std::string lastId;
            uint64_t completedBytes{};
            int lastPercent = -1;
            size_t completedFiles{};
            bool optimizationRequested{};
            std::wstring errorMessage;
            try {
                if (!writeLease->RevalidateMediaLibraryTrust()) {
                    throw std::runtime_error("media library identity changed before import");
                }
                for (auto const& path : files) {
                    if (cancellation->load(std::memory_order_acquire)) break;
                    if (!writeLease->RevalidateMediaLibraryTrust()) {
                        throw std::runtime_error("media library identity changed during import");
                    }
                    uint64_t fileBytes = fs::file_size(path);
                    lastId = library->Import(path, kind, groupId, [&](uint64_t copied, uint64_t) {
                        int percent = totalBytes ? static_cast<int>((completedBytes + copied) * 100 / totalBytes) : 100;
                        if (percent == lastPercent) return;
                        lastPercent = percent;
                        dispatcher.TryEnqueue([weak = get_weak(), percent] {
                            if (auto self = weak.get()) {
                                self->ImportProgress().Value(percent);
                                self->ImportPercentText().Text(to_hstring(percent) + L"%");
                            }
                        });
                    }, cancellation.get());
                    auto importedMedia = library->LoadMedia(groupId);
                    auto imported = std::find_if(importedMedia.begin(), importedMedia.end(), [&](auto const& media) {
                        return media.id == lastId;
                    });
                    if (imported != importedMedia.end()) {
                        library->EnsureCover(*imported);
                        optimizationRequested = library->RequestOptimization(*imported, optimizationMode) || optimizationRequested;
                    }
                    completedBytes += fileBytes;
                    ++completedFiles;
                }
            } catch (...) {
                // Import() aborts its current temporary directory by throwing
                // after observing the cancellation flag. That is a successful
                // user cancellation, not a media-format or I/O failure.
                if (!cancellation->load(std::memory_order_acquire)) {
                    errorMessage = kind == "video"
                        ? L"导入失败。请确认文件格式受支持且媒体库可写。"
                        : L"图片导入失败。请确认格式受 Windows 图像组件支持。";
                }
            }

            bool cancelled = cancellation->load(std::memory_order_acquire);
            if (optimizationRequested) motion::notify_settings_changed();
            dispatcher.TryEnqueue([weak = get_weak(), groupId = std::move(groupId), displayId = std::move(displayId),
                lastId = std::move(lastId), kind = std::move(kind),
                completedFiles, cancelled, optimizationRequested, errorMessage = std::move(errorMessage)]() mutable {
                if (auto self = weak.get()) {
                    self->importing.store(false, std::memory_order_relaxed);
                    self->ImportPanel().Visibility(Visibility::Collapsed);
                    self->ImportImageButton().IsEnabled(true);
                    self->ImportVideoButton().IsEnabled(true);
                    if (!lastId.empty()) {
                        self->SelectWallpaperForTarget(groupId, lastId, displayId);
                        if (!self->TrySaveSettings()) return;
                        self->LoadMedia();
                    }
                    if (!errorMessage.empty()) self->ShowStatus(errorMessage, true);
                    else if (cancelled) self->ShowStatus(completedFiles ? L"导入已取消，已完成的文件已经保留。" : L"导入已取消。");
                    else if (kind == "video") self->ShowStatus(optimizationRequested
                        ? L"视频与首帧封面已导入，性能副本正在后台生成。"
                        : L"视频与首帧封面已导入，当前保留原始文件播放。");
                    else self->ShowStatus(L"静态壁纸已导入，并生成轻量封面缓存。");
                }
            });
        } catch (...) {
            dispatcher.TryEnqueue([weak, kind = std::move(kind)] {
                if (auto self = weak.get()) {
                    self->importing.store(false, std::memory_order_relaxed);
                    self->ImportPanel().Visibility(Visibility::Collapsed);
                    self->ImportImageButton().IsEnabled(true);
                    self->ImportVideoButton().IsEnabled(true);
                    self->ShowStatus(kind == "video" ? L"导入失败。请确认媒体可解码、磁盘空间充足且媒体库可写。" : L"图片导入失败。请确认格式有效、尺寸合理且磁盘空间充足。", true);
                }
            });
        }
    }

    void MainWindow::RenameMedia_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (libraryAccessGate->MigrationInProgress()) {
            ShowStatus(L"媒体库正在迁移，请稍后再重命名壁纸。", true);
            return;
        }
        auto selected = MediaList().SelectedIndex();
        if (selected < 0 || static_cast<size_t>(selected) >= filteredMedia.size()) return;
        auto media = filteredMedia[static_cast<size_t>(selected)];
        TextBox input;
        input.Text(media.name);
        input.SelectAll();
        input.MaxLength(100);
        ContentDialog dialog;
        dialog.XamlRoot(Content().as<FrameworkElement>().XamlRoot());
        dialog.Title(box_value(L"重命名壁纸"));
        dialog.Content(input);
        dialog.PrimaryButtonText(L"保存");
        dialog.CloseButtonText(L"取消");
        dialog.DefaultButton(ContentDialogButton::Primary);
        auto operation = dialog.ShowAsync();
        operation.Completed([weak = get_weak(), media = std::move(media), input](auto const& result, Windows::Foundation::AsyncStatus status) {
            if (status != Windows::Foundation::AsyncStatus::Completed || result.GetResults() != ContentDialogResult::Primary) return;
            if (auto self = weak.get()) {
                auto writeLease = self->TryAcquireLibraryWrite();
                if (!writeLease) return;
                try {
                    self->mediaLibrary->Rename(media, input.Text().c_str());
                    self->LoadMedia();
                    self->ShowStatus(L"壁纸名称已更新。");
                } catch (...) { self->ShowStatus(L"重命名失败，请输入有效名称。", true); }
            }
        });
    }

    void MainWindow::MoveMedia_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (libraryAccessGate->MigrationInProgress()) {
            ShowStatus(L"媒体库正在迁移，请稍后再移动壁纸。", true);
            return;
        }
        auto selected = MediaList().SelectedIndex();
        if (selected < 0 || static_cast<size_t>(selected) >= filteredMedia.size()) return;
        auto media = filteredMedia[static_cast<size_t>(selected)];
        ComboBox picker;
        picker.HorizontalAlignment(HorizontalAlignment::Stretch);
        std::vector<std::string> targets;
        for (auto const& group : groups) {
            if (group.id == media.groupId) continue;
            picker.Items().Append(box_value(group.name));
            targets.push_back(group.id);
        }
        if (targets.empty()) return;
        picker.SelectedIndex(0);
        ContentDialog dialog;
        dialog.XamlRoot(Content().as<FrameworkElement>().XamlRoot());
        dialog.Title(box_value(L"移动到分组"));
        dialog.Content(picker);
        dialog.PrimaryButtonText(L"移动");
        dialog.CloseButtonText(L"取消");
        dialog.DefaultButton(ContentDialogButton::Primary);
        auto operation = dialog.ShowAsync();
        operation.Completed([weak = get_weak(), media = std::move(media), picker, targets = std::move(targets)](auto const& result, Windows::Foundation::AsyncStatus status) {
            if (status != Windows::Foundation::AsyncStatus::Completed || result.GetResults() != ContentDialogResult::Primary) return;
            auto target = picker.SelectedIndex();
            if (target < 0 || static_cast<size_t>(target) >= targets.size()) return;
            if (auto self = weak.get()) self->MoveMedia(std::move(media), targets[static_cast<size_t>(target)]);
        });
    }

    winrt::fire_and_forget MainWindow::MoveMedia(motion::MediaMetadata media, std::string targetId)
    {
        if (activeLibraryMigrationPause) {
            ShowStatus(L"媒体库操作正在等待后台服务，请稍后再试。", true);
            co_return;
        }
        auto writeLease = TryAcquireLibraryWrite();
        if (!writeLease) co_return;
        auto weak = get_weak();
        auto library = mediaLibrary;
        auto dispatcher = DispatcherQueue();
        auto previousSettings = settings;
        try {
            if (settings.selectedGroupId == media.groupId && settings.selectedMediaId == media.id) {
                settings.selectedGroupId = targetId;
            }
            for (auto& assignment : settings.displayAssignments) {
                if (assignment.groupId == media.groupId && assignment.mediaId == media.id) {
                    assignment.groupId = targetId;
                }
            }
            SaveSettings();
        } catch (...) {
            settings = std::move(previousSettings);
            try { SaveSettings(); } catch (...) {}
            ShowStatus(L"无法安全切换壁纸分组；未移动任何文件。", true);
            co_return;
        }

        std::shared_ptr<motion::app::AgentLibraryMigrationPause> agentPause;
        try {
            agentPause = std::make_shared<motion::app::AgentLibraryMigrationPause>();
        } catch (...) {
            settings = std::move(previousSettings);
            ApplySettingsToControls();
            try { SaveSettings(); } catch (...) {}
            ShowStatus(L"无法建立与后台服务的移动协调通道；未移动壁纸。", true);
            co_return;
        }
        activeLibraryMigrationPause = agentPause;
        SetLibraryMigrationUi(true);
        ShowStatus(L"正在暂停后台播放并安全移动壁纸…", false);

        co_await winrt::resume_background();
        bool paused = agentPause->RequestAndWait(std::chrono::seconds(20));
        bool moved{};
        if (paused && !closing.load(std::memory_order_acquire)) {
            try {
                if (!writeLease->RevalidateMediaLibraryTrust()) {
                    throw std::runtime_error("media library identity changed before move");
                }
                library->Move(media, targetId);
                moved = true;
            } catch (...) {}
        }
        bool resumed{};
        if (paused) resumed = agentPause->ResumeAndWait(std::chrono::seconds(20));
        else agentPause->Cancel();

        dispatcher.TryEnqueue([weak, agentPause = std::move(agentPause),
            writeLease = std::move(writeLease), previousSettings = std::move(previousSettings),
            media = std::move(media), targetId = std::move(targetId), paused, moved, resumed]() mutable {
            if (auto self = weak.get()) {
                if (self->activeLibraryMigrationPause == agentPause) {
                    self->activeLibraryMigrationPause.reset();
                }
                writeLease.reset();
                self->SetLibraryMigrationUi(false);
                if (!moved) {
                    self->settings = std::move(previousSettings);
                    self->ApplySettingsToControls();
                    try { self->SaveSettings(); } catch (...) {}
                }
                if (moved) {
                    self->initializing = true;
                    for (size_t index = 0; index < self->groups.size(); ++index) {
                        if (self->groups[index].id == targetId) self->GroupPicker().SelectedIndex(static_cast<int32_t>(index));
                    }
                    self->initializing = false;
                    self->LoadMedia();
                    self->ShowWallpaperPage();
                    self->ShowStatus(resumed
                        ? L"壁纸已移动到新分组。"
                        : L"壁纸已移动，但后台服务未及时确认恢复；请重启应用。", !resumed);
                } else {
                    self->LoadMedia();
                    self->ShowStatus(!paused
                        ? L"后台服务未确认暂停；为保护正在使用的文件，未移动壁纸。"
                        : L"移动已安全中止；媒体库身份可能已变化，未继续按原路径操作。", true);
                }
            }
        });
    }

    void MainWindow::RandomInterval_Changed(IInspectable const&, SelectionChangedEventArgs const&)
    {
        if (initializing) return;
        auto previousSettings = settings;
        int interval = combo_int(RandomInterval(), -1);
        bool enabled = interval >= 0 && motion::valid_id(settings.selectedGroupId);
        settings.randomIntervalMinutes = enabled ? interval : 0;
        settings.randomGroupId = enabled ? settings.selectedGroupId : std::string{};
        if (!TrySaveSettings()) {
            settings = std::move(previousSettings);
            ApplySettingsToControls();
            return;
        }
        ShowStatus(!enabled
            ? L"分组随机播放已关闭。"
            : settings.randomIntervalMinutes
                ? L"当前壁纸分组将按所选间隔随机切换，并避免连续重复。"
                : L"当前壁纸分组将在启动或解锁时随机切换。");
    }

    void MainWindow::RequestVariant(motion::MediaMetadata const& media, std::string const& mode)
    {
        auto writeLease = TryAcquireLibraryWrite();
        if (!writeLease) return;
        try {
            if (!mediaLibrary->RequestOptimization(media, mode)) {
                ShowStatus(L"无法创建优化任务。", true);
                return;
            }
            motion::notify_settings_changed();
            RefreshVariants();
            ShowStatus(L"已创建性能副本生成任务；完成后可选择是否保留源文件。");
        } catch (...) {
            ShowStatus(L"无法创建优化任务，请检查媒体库是否可写。", true);
        }
    }

    void MainWindow::SetVariantPaused(motion::MediaMetadata const& media, bool paused)
    {
        auto writeLease = TryAcquireLibraryWrite();
        if (!writeLease) return;
        try {
            if (paused) mediaLibrary->PauseOptimization(media);
            else mediaLibrary->ResumeOptimization(media);
            motion::notify_settings_changed();
            RefreshVariants();
            ShowStatus(paused
                ? L"性能副本任务已暂停；继续时会从头安全生成。"
                : L"性能副本任务已继续。");
        } catch (...) {
            ShowStatus(paused ? L"暂停性能副本任务失败。" : L"继续性能副本任务失败。", true);
        }
    }

    void MainWindow::CancelVariant(motion::MediaMetadata const& media)
    {
        auto writeLease = TryAcquireLibraryWrite();
        if (!writeLease) return;
        try {
            mediaLibrary->CancelOptimization(media);
            motion::notify_settings_changed();
            RefreshVariants();
            ShowStatus(L"已取消任务并清理本次临时文件；已经完成的副本不会删除。");
        } catch (...) {
            ShowStatus(L"取消优化任务失败。", true);
        }
    }

    void MainWindow::ConfirmDeleteVariantSelection(motion::MediaMetadata const& media,
        uint8_t selection)
    {
        auto writeLease = TryAcquireLibraryWrite();
        if (!writeLease) return;
        if (selection & variant_source) {
            auto status = mediaLibrary->VariantStatus(media);
            if (!mediaLibrary->SourceAvailable(media) || status.entries.empty()) {
                ShowStatus(L"至少需要一个完整的性能副本，才能删除源文件。", true);
                return;
            }
            ContentDialog dialog;
            dialog.XamlRoot(Content().as<FrameworkElement>().XamlRoot());
            dialog.Title(box_value(L"删除壁纸源文件？"));
            dialog.Content(box_value(L"将把 " + format_size(media.sizeBytes) +
                L" 的源媒体移入 Windows 回收站。性能副本、首帧和名称会保留；之后无法选择原画，也无法重新生成性能副本。"));
            dialog.PrimaryButtonText(L"删除源文件");
            dialog.CloseButtonText(L"取消");
            dialog.DefaultButton(ContentDialogButton::Close);
            auto operation = dialog.ShowAsync();
            operation.Completed([weak = get_weak(), media](auto const& result,
                Windows::Foundation::AsyncStatus state) {
                if (state != Windows::Foundation::AsyncStatus::Completed ||
                    result.GetResults() != ContentDialogResult::Primary) return;
                if (auto self = weak.get()) self->DeleteSource(media);
            });
            return;
        }

        selection &= variant_balanced | variant_power_saver;
        if (!selection) return;
        auto status = mediaLibrary->VariantStatus(media);
        uint64_t selectedBytes{};
        uint32_t selectedFiles{};
        bool selectedSharedStorage{};
        for (auto const& entry : status.entries) {
            bool selectedProfile = (entry.mode == "balanced" && (selection & variant_balanced)) ||
                (entry.mode == "power-saver" && (selection & variant_power_saver));
            if (!selectedProfile) continue;
            selectedBytes += entry.bytes;
            ++selectedFiles;
            selectedSharedStorage = selectedSharedStorage || entry.sharedStorage;
        }
        bool active = ((status.requestedMode == "balanced" && (selection & variant_balanced)) ||
            (status.requestedMode == "power-saver" && (selection & variant_power_saver))) &&
            (status.queued || status.generating);
        if (!selectedFiles && !active) return;
        bool sourceAvailable = mediaLibrary->SourceAvailable(media);
        if (!sourceAvailable && selectedFiles >= status.entries.size()) {
            ContentDialog blocked;
            blocked.XamlRoot(Content().as<FrameworkElement>().XamlRoot());
            blocked.Title(box_value(L"至少保留一个性能副本"));
            blocked.Content(box_value(L"源文件已经删除，所选项目包含这张壁纸最后的可播放文件。要全部移除，请在“我的壁纸”中删除整张壁纸。"));
            blocked.CloseButtonText(L"知道了");
            blocked.ShowAsync();
            return;
        }

        ContentDialog dialog;
        dialog.XamlRoot(Content().as<FrameworkElement>().XamlRoot());
        std::wstring profileName;
        if ((selection & variant_balanced) && (selection & variant_power_saver)) profileName = L"自动平衡和低功耗";
        else profileName = selection & variant_balanced ? L"自动平衡" : L"低功耗";
        dialog.Title(box_value(L"删除" + profileName + L"副本？"));
        if (selectedSharedStorage && (selection & variant_balanced) && (selection & variant_power_saver)) {
            selectedBytes = status.bytes;
        }
        auto detail = selectedFiles
            ? selectedSharedStorage && !((selection & variant_balanced) && (selection & variant_power_saver))
                ? L"将删除" + profileName + L"档位；它与另一档位共享同一份文件，不会重复释放磁盘空间。"
                : L"将永久删除 " + format_size(selectedBytes) + L" 的" + profileName + L"副本。"
            : L"将取消正在进行的" + profileName + L"任务。";
        detail += sourceAvailable
            ? L"源文件和未选择的副本会保留；所选副本不会自动重新生成。"
            : L"未选择的副本会继续作为壁纸播放；没有源文件时无法重新生成。";
        dialog.Content(box_value(detail));
        dialog.PrimaryButtonText(L"删除所选");
        dialog.CloseButtonText(L"取消");
        dialog.DefaultButton(ContentDialogButton::Close);
        auto operation = dialog.ShowAsync();
        operation.Completed([weak = get_weak(), media, selection](auto const& result,
            Windows::Foundation::AsyncStatus state) {
            if (state != Windows::Foundation::AsyncStatus::Completed || result.GetResults() != ContentDialogResult::Primary) return;
            if (auto self = weak.get()) self->DeleteVariantProfiles(media, selection);
        });
    }

    winrt::fire_and_forget MainWindow::DeleteVariantProfiles(motion::MediaMetadata media, uint8_t selection)
    {
        if (activeLibraryMigrationPause) {
            ShowStatus(L"媒体库操作正在等待后台服务，请稍后再试。", true);
            co_return;
        }
        auto writeLease = TryAcquireLibraryWrite();
        if (!writeLease) co_return;
        auto weak = get_weak();
        auto library = mediaLibrary;
        auto dispatcher = DispatcherQueue();
        bool sourceAvailable = library->SourceAvailable(media);
        auto previousSettings = settings;
        bool settingsChanged{};
        std::vector<std::string> modes;
        try {
            auto status = library->VariantStatus(media);
            if (!sourceAvailable) {
                bool balancedRemains = !(selection & variant_balanced) &&
                    std::any_of(status.entries.begin(), status.entries.end(),
                        [](auto const& entry) { return entry.mode == "balanced" && entry.bytes; });
                bool powerSaverRemains = !(selection & variant_power_saver) &&
                    std::any_of(status.entries.begin(), status.entries.end(),
                        [](auto const& entry) { return entry.mode == "power-saver" && entry.bytes; });
                auto fallback = balancedRemains ? std::string("balanced")
                    : powerSaverRemains ? std::string("power-saver") : std::string{};
                if (fallback.empty()) throw std::runtime_error("cannot delete the last playable copy");
                bool currentRemoved = settings.performanceMode == "original" ||
                    (settings.performanceMode == "balanced" && (selection & variant_balanced)) ||
                    (settings.performanceMode == "power-saver" && (selection & variant_power_saver));
                if (currentRemoved) {
                    settings.performanceMode = fallback;
                    bool wasInitializing = initializing;
                    initializing = true;
                    select_tag(PerformanceMode(), motion::utf8_to_wide(fallback));
                    initializing = wasInitializing;
                    if (!TrySaveSettings()) throw std::runtime_error("cannot save fallback performance mode");
                    settingsChanged = true;
                }
            }
            if (selection & variant_balanced) modes.push_back("balanced");
            if (selection & variant_power_saver) modes.push_back("power-saver");
            if (modes.empty()) throw std::runtime_error("no variant profile selected");
        } catch (...) {
            if (settingsChanged) {
                settings = std::move(previousSettings);
                ApplySettingsToControls();
                try { SaveSettings(); } catch (...) {}
            }
            ShowStatus(L"无法删除副本；请确认至少保留一个可播放文件且副本未被占用。", true);
            co_return;
        }

        std::shared_ptr<motion::app::AgentLibraryMigrationPause> agentPause;
        try {
            agentPause = std::make_shared<motion::app::AgentLibraryMigrationPause>();
        } catch (...) {
            if (settingsChanged) {
                settings = std::move(previousSettings);
                ApplySettingsToControls();
                try { SaveSettings(); } catch (...) {}
            }
            ShowStatus(L"无法建立与后台服务的删除协调通道；未删除任何副本。", true);
            co_return;
        }
        activeLibraryMigrationPause = agentPause;
        SetLibraryMigrationUi(true);
        ShowStatus(L"正在暂停后台播放并安全删除所选副本…", false);

        co_await winrt::resume_background();
        bool paused = agentPause->RequestAndWait(std::chrono::seconds(20));
        bool deleted{};
        if (paused && !closing.load(std::memory_order_acquire)) {
            try {
                if (!writeLease->RevalidateMediaLibraryTrust()) {
                    throw std::runtime_error("media library identity changed before variant deletion");
                }
                library->DeleteVariantProfiles(media, modes);
                deleted = true;
            } catch (...) {}
        }
        bool resumed{};
        if (paused) resumed = agentPause->ResumeAndWait(std::chrono::seconds(20));
        else agentPause->Cancel();

        dispatcher.TryEnqueue([weak, agentPause = std::move(agentPause),
            writeLease = std::move(writeLease), previousSettings = std::move(previousSettings),
            settingsChanged, mediaId = media.id, sourceAvailable, paused, deleted, resumed]() mutable {
            if (auto self = weak.get()) {
                if (self->activeLibraryMigrationPause == agentPause) self->activeLibraryMigrationPause.reset();
                writeLease.reset();
                self->SetLibraryMigrationUi(false);
                if (!deleted && settingsChanged) {
                    self->settings = std::move(previousSettings);
                    self->ApplySettingsToControls();
                    try { self->SaveSettings(); } catch (...) {}
                }
                self->variantSelections.erase(mediaId);
                self->RefreshVariants();
                if (!paused) {
                    self->ShowStatus(L"后台服务未确认暂停；为保护正在使用的文件，未删除任何副本。", true);
                } else if (!deleted) {
                    self->ShowStatus(L"删除已安全中止；媒体库身份可能已变化，未继续按原路径操作。", true);
                } else if (!resumed) {
                    self->ShowStatus(L"副本已删除，但后台服务未及时确认恢复；请重启应用。", true);
                } else {
                    self->ShowStatus(sourceAvailable
                        ? L"所选性能副本已删除，源文件和其他副本已保留。"
                        : L"所选性能副本已删除，剩余副本会继续播放。");
                }
            }
        });
    }

    winrt::fire_and_forget MainWindow::DeleteSource(motion::MediaMetadata media)
    {
        if (activeLibraryMigrationPause) {
            ShowStatus(L"媒体库操作正在等待后台服务，请稍后再试。", true);
            co_return;
        }
        auto writeLease = TryAcquireLibraryWrite();
        if (!writeLease) co_return;
        auto weak = get_weak();
        auto library = mediaLibrary;
        auto dispatcher = DispatcherQueue();
        auto previousSettings = settings;
        bool settingsChanged{};
        try {
            auto status = library->VariantStatus(media);
            bool balancedAvailable = std::any_of(status.entries.begin(), status.entries.end(),
                [](auto const& entry) { return entry.mode == "balanced" && entry.bytes; });
            bool powerSaverAvailable = std::any_of(status.entries.begin(), status.entries.end(),
                [](auto const& entry) { return entry.mode == "power-saver" && entry.bytes; });
            if (!balancedAvailable && !powerSaverAvailable) {
                throw std::runtime_error("no playable performance copy exists");
            }
            bool selectedModeAvailable =
                (settings.performanceMode == "balanced" && balancedAvailable) ||
                (settings.performanceMode == "power-saver" && powerSaverAvailable);
            if (!selectedModeAvailable) {
                settings.performanceMode = balancedAvailable ? "balanced" : "power-saver";
                bool wasInitializing = initializing;
                initializing = true;
                select_tag(PerformanceMode(), motion::utf8_to_wide(settings.performanceMode));
                initializing = wasInitializing;
                if (!TrySaveSettings()) throw std::runtime_error("cannot save fallback performance mode");
                settingsChanged = true;
            }
        } catch (...) {
            if (settingsChanged) {
                settings = std::move(previousSettings);
                ApplySettingsToControls();
                try { SaveSettings(); } catch (...) {}
            }
            ShowStatus(L"无法删除源文件；请确认副本完整且文件未被占用。", true);
            co_return;
        }

        std::shared_ptr<motion::app::AgentLibraryMigrationPause> agentPause;
        try {
            agentPause = std::make_shared<motion::app::AgentLibraryMigrationPause>();
        } catch (...) {
            if (settingsChanged) {
                settings = std::move(previousSettings);
                ApplySettingsToControls();
                try { SaveSettings(); } catch (...) {}
            }
            ShowStatus(L"无法建立与后台服务的删除协调通道；未删除源文件。", true);
            co_return;
        }
        activeLibraryMigrationPause = agentPause;
        SetLibraryMigrationUi(true);
        ShowStatus(L"正在暂停后台播放并安全删除源文件…", false);

        co_await winrt::resume_background();
        bool paused = agentPause->RequestAndWait(std::chrono::seconds(20));
        bool deleted{};
        if (paused && !closing.load(std::memory_order_acquire)) {
            try {
                if (!writeLease->RevalidateMediaLibraryTrust()) {
                    throw std::runtime_error("media library identity changed before source deletion");
                }
                library->DeleteSource(media);
                deleted = true;
            } catch (...) {}
        }
        bool resumed{};
        if (paused) resumed = agentPause->ResumeAndWait(std::chrono::seconds(20));
        else agentPause->Cancel();

        dispatcher.TryEnqueue([weak, agentPause = std::move(agentPause),
            writeLease = std::move(writeLease), previousSettings = std::move(previousSettings),
            settingsChanged, mediaId = media.id, paused, deleted, resumed]() mutable {
            if (auto self = weak.get()) {
                if (self->activeLibraryMigrationPause == agentPause) self->activeLibraryMigrationPause.reset();
                writeLease.reset();
                self->SetLibraryMigrationUi(false);
                if (!deleted && settingsChanged) {
                    self->settings = std::move(previousSettings);
                    self->ApplySettingsToControls();
                    try { self->SaveSettings(); } catch (...) {}
                }
                self->variantSelections.erase(mediaId);
                self->variantViewFingerprint.clear();
                self->RefreshVariants();
                self->LoadMedia();
                self->UpdateStatusSummary();
                if (!paused) {
                    self->ShowStatus(L"后台服务未确认暂停；为保护正在使用的文件，未删除源文件。", true);
                } else if (!deleted) {
                    self->ShowStatus(L"删除已安全中止；媒体库身份可能已变化，未继续按原路径操作。", true);
                } else if (!resumed) {
                    self->ShowStatus(L"源文件已删除，但后台服务未及时确认恢复；请重启应用。", true);
                } else {
                    self->ShowStatus(L"源文件已移入回收站；性能副本、首帧和名称已保留。");
                }
            }
        });
    }

    void MainWindow::DeleteMedia_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (libraryAccessGate->MigrationInProgress()) {
            ShowStatus(L"媒体库正在迁移，请稍后再删除壁纸。", true);
            return;
        }
        auto selected = MediaList().SelectedIndex();
        if (selected < 0 || static_cast<size_t>(selected) >= filteredMedia.size()) return;
        auto media = filteredMedia[static_cast<size_t>(selected)];
        ContentDialog dialog;
        dialog.XamlRoot(Content().as<FrameworkElement>().XamlRoot());
        dialog.Title(box_value(L"删除壁纸本体？"));
        dialog.Content(box_value(L"源媒体、优化副本、封面和元数据会一起移入 Windows 回收站，可在回收站中恢复。"));
        dialog.PrimaryButtonText(L"移到回收站");
        dialog.CloseButtonText(L"取消");
        dialog.DefaultButton(ContentDialogButton::Close);
        auto operation = dialog.ShowAsync();
        operation.Completed([weak = get_weak(), media = std::move(media)](auto const& result, Windows::Foundation::AsyncStatus status) {
            if (status != Windows::Foundation::AsyncStatus::Completed || result.GetResults() != ContentDialogResult::Primary) return;
            if (auto self = weak.get()) self->DeleteMedia(media);
        });
    }

    winrt::fire_and_forget MainWindow::DeleteMedia(motion::MediaMetadata media)
    {
        if (activeLibraryMigrationPause) {
            ShowStatus(L"媒体库操作正在等待后台服务，请稍后再试。", true);
            co_return;
        }
        auto writeLease = TryAcquireLibraryWrite();
        if (!writeLease) co_return;
        auto weak = get_weak();
        auto library = mediaLibrary;
        auto dispatcher = DispatcherQueue();
        auto previousSettings = settings;
        try {
            if (settings.selectedGroupId == media.groupId && settings.selectedMediaId == media.id) {
                settings.selectedMediaId.clear();
            }
            RemoveMediaAssignments(media.groupId, media.id);
            SaveSettings();
            motion::notify_settings_changed();
        } catch (...) {
            settings = std::move(previousSettings);
            try { SaveSettings(); } catch (...) {}
            LoadMedia();
            ShowStatus(L"无法停止正在进行的媒体任务，请稍后重试。", true);
            co_return;
        }

        std::shared_ptr<motion::app::AgentLibraryMigrationPause> agentPause;
        try {
            agentPause = std::make_shared<motion::app::AgentLibraryMigrationPause>();
        } catch (...) {
            settings = std::move(previousSettings);
            ApplySettingsToControls();
            try { SaveSettings(); } catch (...) {}
            ShowStatus(L"无法建立与后台服务的删除协调通道；未删除壁纸。", true);
            co_return;
        }
        activeLibraryMigrationPause = agentPause;
        SetLibraryMigrationUi(true);
        ShowStatus(L"正在暂停后台播放并安全删除壁纸…", false);

        co_await winrt::resume_background();
        bool paused = agentPause->RequestAndWait(std::chrono::seconds(20));
        bool deleted{};
        if (paused && !closing.load(std::memory_order_acquire)) {
            try {
                if (!writeLease->RevalidateMediaLibraryTrust()) {
                    throw std::runtime_error("media library identity changed before wallpaper deletion");
                }
                library->CancelOptimization(media);
                if (!writeLease->RevalidateMediaLibraryTrust()) {
                    throw std::runtime_error("media library identity changed after optimizer cancellation");
                }
                library->Delete(media);
                deleted = true;
            } catch (...) {}
        }
        bool resumed{};
        if (paused) resumed = agentPause->ResumeAndWait(std::chrono::seconds(20));
        else agentPause->Cancel();

        dispatcher.TryEnqueue([weak, agentPause = std::move(agentPause),
            writeLease = std::move(writeLease), previousSettings = std::move(previousSettings),
            mediaId = media.id, paused, deleted, resumed]() mutable {
            if (auto self = weak.get()) {
                if (self->activeLibraryMigrationPause == agentPause) self->activeLibraryMigrationPause.reset();
                writeLease.reset();
                self->SetLibraryMigrationUi(false);
                if (!deleted) {
                    self->settings = std::move(previousSettings);
                    self->ApplySettingsToControls();
                    try { self->SaveSettings(); } catch (...) {}
                }
                self->variantSelections.erase(mediaId);
                self->LoadMedia();
                if (self->currentPage == AppPage::Variants) self->RefreshVariants();
                if (!paused) {
                    self->ShowStatus(L"后台服务未确认暂停；为保护正在使用的文件，未删除壁纸。", true);
                } else if (!deleted) {
                    self->ShowStatus(L"删除已安全中止；媒体库身份可能已变化，未继续按原路径操作。", true);
                } else if (!resumed) {
                    self->ShowStatus(L"壁纸已删除，但后台服务未及时确认恢复；请重启应用。", true);
                } else {
                    self->ShowStatus(L"壁纸本体及其优化副本已移到 Windows 回收站。");
                }
            }
        });
    }

    void MainWindow::OpenLibrary_Click(IInspectable const&, RoutedEventArgs const&)
    {
        auto access = TryAcquireLibraryWrite();
        if (!access) return;
        auto path = mediaLibrary->WallpapersPath();
        ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    void MainWindow::MoveLibrary_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (!settingsWritable) {
            ShowStatus(L"当前设置文件无法安全读取，不能迁移媒体库；请修复设置文件后重启应用。", true);
            return;
        }
        if (libraryAccessGate->MigrationInProgress()) {
            ShowStatus(L"媒体库迁移已经在进行中。", true);
            return;
        }
        try {
            HWND window{};
            auto nativeWindow = this->try_as<::IWindowNative>();
            check_hresult(nativeWindow->get_WindowHandle(&window));
            auto target = select_folder(window, mediaLibraryAvailable
                ? L"选择新的媒体库文件夹（必须为空）"
                : L"连接已有 MotionWallpaper 媒体库（不会在空文件夹中新建）");
            if (!target.empty()) MoveLibrary(std::move(target));
        } catch (...) {
            ShowStatus(L"无法选择媒体库位置。", true);
        }
    }

    winrt::fire_and_forget MainWindow::MoveLibrary(fs::path target)
    {
        auto weak = get_weak();
        auto dispatcher = DispatcherQueue();
        auto source = mediaLibrary->WallpapersPath();
        bool connectExistingLibrary = !mediaLibraryAvailable;
        if (!connectExistingLibrary) {
            try {
                if (!settings.mediaLibraryPath.empty() &&
                    (!mediaLibraryTrust ||
                        mediaLibraryTrust->ownershipId != settings.mediaLibraryId)) {
                    mediaLibraryTrust.reset();
                    mediaLibraryAvailable = false;
                    if (!motion::notify_settings_changed()) StartController();
                    ShowStatus(L"当前媒体库身份或所有权已变化，不能从该路径迁移。请选择连接一个已有的有效媒体库。", true);
                    co_return;
                }
                if (motion::same_filesystem_path(source, target)) {
                    ShowStatus(L"新位置与当前媒体库相同。", true);
                    co_return;
                }
            } catch (...) {
                ShowStatus(L"无法安全解析媒体库位置，迁移没有开始。", true);
                co_return;
            }
        }
        auto migrationLease = libraryAccessGate->TryBeginMigration();
        if (!migrationLease) {
            ShowStatus(L"仍有导入、封面、移动或删除任务正在使用媒体库，请等待完成后重试。", true);
            co_return;
        }

        std::optional<motion::MediaLibraryTrustIdentity> sourceIdentity;
        std::shared_ptr<motion::MediaLibraryTrustLease> sourceTrust;
        if (!connectExistingLibrary) {
            sourceIdentity = settings.mediaLibraryPath.empty()
                ? motion::capture_media_library_trust(source)
                : mediaLibraryTrust;
            if (!settings.mediaLibraryPath.empty() && sourceIdentity &&
                sourceIdentity->ownershipId != settings.mediaLibraryId) {
                sourceIdentity.reset();
            }
            sourceTrust = sourceIdentity
                ? motion::acquire_media_library_trust(*sourceIdentity)
                : std::shared_ptr<motion::MediaLibraryTrustLease>{};
            if (!sourceTrust) {
                migrationLease.reset();
                if (!settings.mediaLibraryPath.empty()) {
                    mediaLibraryTrust.reset();
                    mediaLibraryAvailable = false;
                    if (!motion::notify_settings_changed()) StartController();
                }
                ShowStatus(L"当前媒体库身份或所有权已变化，迁移没有开始。", true);
                co_return;
            }
        }

        std::shared_ptr<motion::app::AgentLibraryMigrationPause> agentPause;
        try {
            agentPause = std::make_shared<motion::app::AgentLibraryMigrationPause>();
        } catch (...) {
            migrationLease.reset();
            ShowStatus(L"无法建立与后台服务的迁移协调通道。", true);
            co_return;
        }
        auto cancellation = std::make_shared<std::atomic_bool>();
        activeLibraryMigrationPause = agentPause;
        libraryMigrationCancellation = cancellation;
        SetLibraryMigrationUi(true);
        ShowStatus(connectExistingLibrary
            ? L"正在安全暂停壁纸服务并验证已有媒体库…"
            : L"正在安全暂停壁纸服务…", false);
        if (!motion::notify_settings_changed()) StartController();

        std::shared_ptr<motion::app::LibraryMigrationTransaction> transaction;
        std::optional<motion::MediaLibraryTrustIdentity> targetTrust;
        std::wstring failure;
        try {
            co_await winrt::resume_background();
            if (!agentPause->RequestAndWait(std::chrono::seconds(20))) {
                failure = L"后台壁纸服务未能确认暂停，迁移没有开始。请重启应用后重试。";
            } else if (cancellation->load(std::memory_order_acquire)) {
                failure = L"媒体库迁移已取消，原位置保持不变。";
            } else if (connectExistingLibrary) {
                target = fs::absolute(target).lexically_normal();
                auto identity = motion::capture_media_library_trust(target);
                if (identity && motion::is_owned_media_library(target) &&
                    motion::revalidate_media_library_trust(*identity)) {
                    targetTrust = std::move(*identity);
                    // Safe reconnect/switch: the unavailable source is neither
                    // traversed nor cleaned and the existing target is not
                    // modified before it becomes the configured library.
                } else {
                    failure = L"原媒体库当前离线。只能连接一个已存在、带 MotionWallpaper 所有权标记且不含重解析点的媒体库；未创建或修改所选文件夹。";
                }
            } else {
                if (!sourceIdentity || !sourceTrust ||
                    !motion::revalidate_media_library_trust(*sourceIdentity)) {
                    throw std::runtime_error("source media library identity changed before migration");
                }
                transaction = motion::app::LibraryMigrationTransaction::Begin(
                    source, target, *sourceIdentity);
                // The transaction acquired its own lease before this
                // pre-pause lease is released, leaving no unguarded copy gap.
                sourceTrust.reset();
                auto lastPercent = std::make_shared<std::atomic_int>(-1);
                transaction->CopyAndVerify([dispatcher, weak, lastPercent](uint64_t copied, uint64_t total) {
                    int percent = total ? static_cast<int>((std::min)(
                        static_cast<long double>(copied) * 100.0L / static_cast<long double>(total), 100.0L)) : 100;
                    if (lastPercent->exchange(percent, std::memory_order_acq_rel) == percent) return;
                    dispatcher.TryEnqueue([weak, percent] {
                        if (auto self = weak.get(); self && !self->closing.load(std::memory_order_acquire)) {
                            self->ShowStatus(L"正在复制并逐文件校验媒体库（" +
                                std::to_wstring(percent) + L"%）…", false);
                        }
                    });
                }, cancellation.get());
                transaction->CommitPreparedTarget();
                auto identity = motion::capture_media_library_trust(transaction->Target());
                if (!identity || !motion::is_owned_media_library(transaction->Target()) ||
                    !motion::revalidate_media_library_trust(*identity)) {
                    throw std::runtime_error("committed media library identity changed");
                }
                targetTrust = std::move(*identity);
            }
        } catch (...) {
            failure = L"迁移失败。请确认当前媒体库结构完整、目标文件夹为空，且目标磁盘空间充足可写；原媒体库未改动。";
        }

        dispatcher.TryEnqueue([weak, transaction = std::move(transaction), agentPause = std::move(agentPause),
            migrationLease = std::move(migrationLease), cancellation = std::move(cancellation),
            failure = std::move(failure), target = std::move(target),
            targetTrust = std::move(targetTrust), connectExistingLibrary]() mutable {
                auto self = weak.get();
                if (!self || self->closing.load(std::memory_order_acquire)) {
                    agentPause->Cancel();
                    return;
                }
                if (!failure.empty() || !targetTrust || (!connectExistingLibrary && !transaction)) {
                    agentPause->Cancel();
                    if (self->activeLibraryMigrationPause == agentPause) self->activeLibraryMigrationPause.reset();
                    migrationLease.reset();
                    self->SetLibraryMigrationUi(false);
                    self->ShowStatus(failure.empty() ? L"媒体库迁移失败，原位置保持不变。" : failure, true);
                    return;
                }

                auto previousPath = self->settings.mediaLibraryPath;
                auto previousLibraryId = self->settings.mediaLibraryId;
                auto previousLibrary = self->mediaLibrary;
                auto previousLibraryAvailable = self->mediaLibraryAvailable;
                auto previousLibraryTrust = self->mediaLibraryTrust;
                try {
                    auto destination = connectExistingLibrary ? target : transaction->Target();
                    auto trustLease = motion::acquire_media_library_trust(*targetTrust);
                    if (!trustLease) throw std::runtime_error("target media library identity changed");
                    auto newLibrary = std::make_shared<motion::app::MediaLibrary>(self->root,
                        motion::app::DeleteMode::RecycleBin, destination, *targetTrust);
                    self->settings.mediaLibraryPath = destination.wstring();
                    self->settings.mediaLibraryId = targetTrust->ownershipId;
                    self->SaveSettings();
                    self->mediaLibrary = std::move(newLibrary);
                    self->mediaLibraryTrust = std::move(*targetTrust);
                    self->mediaLibraryAvailable = true;
                    if (transaction) transaction->MarkActivated();
                    self->LibraryPath().Text(destination.wstring());
                    self->LibraryPathFull().Text(destination.wstring());
                    self->ShowStatus(L"新媒体库已启用，正在等待后台服务确认切换…", false);
                    self->FinalizeLibraryMigration(std::move(transaction), std::move(agentPause),
                        std::move(migrationLease), std::move(cancellation));
                } catch (...) {
                    self->settings.mediaLibraryPath = std::move(previousPath);
                    self->settings.mediaLibraryId = std::move(previousLibraryId);
                    self->mediaLibrary = std::move(previousLibrary);
                    self->mediaLibraryAvailable = previousLibraryAvailable;
                    self->mediaLibraryTrust = std::move(previousLibraryTrust);
                    try { self->SaveSettings(); } catch (...) {}
                    agentPause->Cancel();
                    if (self->activeLibraryMigrationPause == agentPause) self->activeLibraryMigrationPause.reset();
                    migrationLease.reset();
                    self->SetLibraryMigrationUi(false);
                    self->ShowStatus(L"无法启用新媒体库；设置和原位置保持不变。", true);
                }
            });
    }

    winrt::fire_and_forget MainWindow::FinalizeLibraryMigration(
        std::shared_ptr<motion::app::LibraryMigrationTransaction> transaction,
        std::shared_ptr<motion::app::AgentLibraryMigrationPause> agentPause,
        std::shared_ptr<motion::app::LibraryMigrationLease> migrationLease,
        std::shared_ptr<std::atomic_bool> cancellation)
    {
        auto weak = get_weak();
        auto dispatcher = DispatcherQueue();
        co_await winrt::resume_background();
        bool applied{};
        fs::path backup;
        if (!cancellation->load(std::memory_order_acquire)) {
            applied = agentPause->ResumeAndWait(std::chrono::seconds(20));
            if (transaction && applied && !cancellation->load(std::memory_order_acquire)) {
                try { backup = transaction->ArchiveVerifiedSource(); }
                catch (...) {}
            }
        } else {
            agentPause->Cancel();
        }

        dispatcher.TryEnqueue([weak, transaction = std::move(transaction), agentPause = std::move(agentPause),
            migrationLease = std::move(migrationLease), cancellation = std::move(cancellation),
            applied, backup = std::move(backup)]() mutable {
            if (auto self = weak.get(); self && !self->closing.load(std::memory_order_acquire)) {
                if (self->activeLibraryMigrationPause == agentPause) self->activeLibraryMigrationPause.reset();
                migrationLease.reset();
                self->SetLibraryMigrationUi(false);
                self->variantSelections.clear();
                self->variantViewFingerprint.clear();
                self->LoadGroups();
                self->LoadMedia();
                if (self->currentPage == AppPage::Variants) self->RefreshVariants();
                if (!applied) {
                    auto detail = transaction
                        ? L"；旧媒体库已保留在原位置。请重启应用确认后再手动处理旧库：" + transaction->Source().wstring()
                        : L"。原离线库的路径和数据均未清理";
                    self->ShowStatus(L"新媒体库已保存，但后台服务未及时确认切换" +
                        detail, true);
                } else if (!transaction) {
                    self->ShowStatus(L"已安全连接所选的现有媒体库；原离线库的数据未读取、迁移或清理。", false);
                } else if (backup.empty()) {
                    self->ShowStatus(L"媒体库已切换；旧媒体库未能归档，仍安全保留在：" +
                        transaction->Source().wstring(), true);
                } else {
                    self->ShowStatus(L"媒体库已安全迁移。旧库已作为可恢复备份保留在：" + backup.wstring(), false);
                }
            }
        });
    }

    void MainWindow::StartController()
    {
        auto executable = applicationRoot / L"motionwallpaper-agent.exe";
        if (!fs::exists(executable)) return;
        std::wstring command = L"\"" + executable.wstring() + L"\"";
        STARTUPINFOW startup{ sizeof(startup) };
        PROCESS_INFORMATION process{};
        if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
            nullptr, applicationRoot.c_str(), &startup, &process)) {
            motion::unique_handle thread(process.hThread);
            motion::unique_handle handle(process.hProcess);
        }
    }

    void MainWindow::ShowStatus(std::wstring const& message, bool error)
    {
        statusHideTimer.Stop();
        StatusBar().Severity(error ? InfoBarSeverity::Error : InfoBarSeverity::Success);
        StatusBar().Message(message);
        StatusBar().IsOpen(true);
        statusHideTimer.Start();
    }
}

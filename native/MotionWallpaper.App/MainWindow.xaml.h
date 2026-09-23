#pragma once

#include "MainWindow.g.h"
#include "LibraryBackup.h"
#include "LibraryMigration.h"
#include "MediaLibrary.h"
#include "SettingsStore.h"
#include "VariantTaskView.h"
#include "../MotionWallpaper.Common/DisplayTopology.h"
#include "../MotionWallpaper.Common/SceneProfiles.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>

namespace winrt::MotionWallpaper::implementation
{
    struct MainWindow : MainWindowT<MainWindow>
    {
        MainWindow();

        void Settings_Changed(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void Policy_Changed(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void SystemSettings_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void Variants_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void GroupPicker_SelectionChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void GroupPicker_RightTapped(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Input::RightTappedRoutedEventArgs const&);
        void GroupPicker_DragItemsStarting(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::DragItemsStartingEventArgs const&);
        void GroupPicker_DragItemsCompleted(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::DragItemsCompletedEventArgs const&);
        void Media_SelectionChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void DisplayTargetPicker_Changed(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void ImportVideo_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void ImportImage_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void CancelImport_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void DeleteMedia_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void RenameMedia_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void MoveMedia_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void RandomInterval_Changed(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void Sort_Changed(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void MediaSearch_Changed(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::TextChangedEventArgs const&);
        void MediaFilter_Changed(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void MediaFilter_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void BatchMode_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void ToggleFavorite_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void EditTags_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void RepairDuplicates_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OpenNewGroup_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void CreateGroup_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void RenameGroup_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void MoveGroupUp_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void MoveGroupDown_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void DeleteGroup_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OpenLibrary_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void MoveLibrary_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void CurrentWallpaper_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void RetryRuntime_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void RestartRenderer_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void PreviewScreensaver_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void ApplyOptimizationQuota_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void ReleaseOptimizationSpace_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void BackupLibrary_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void RestoreLibrary_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void ScenePicker_Changed(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void ApplyScene_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void SaveScene_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void SceneAutoSwitch_Changed(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);

    private:
        enum class AppPage { Settings, Variants, WallpaperGroup };

        std::filesystem::path root;
        std::filesystem::path applicationRoot;
        std::unique_ptr<motion::app::SettingsStore> settingsStore;
        std::shared_ptr<motion::app::MediaLibrary> mediaLibrary;
        std::shared_ptr<motion::app::LibraryAccessGate> libraryAccessGate{
            std::make_shared<motion::app::LibraryAccessGate>() };
        std::shared_ptr<motion::app::AgentLibraryMigrationPause> activeLibraryMigrationPause;
        std::shared_ptr<std::atomic_bool> libraryMigrationCancellation{
            std::make_shared<std::atomic_bool>() };
        motion::Settings settings;
        motion::RuntimeState runtimeState;
        std::optional<motion::MediaMetadata> optimizationSummaryMedia;
        std::string appliedGroupId;
        std::string appliedMediaId;
        std::string actualDecodePath;
        std::string actualDecodeReason;
        std::string pendingRuntimeCommandId;
        std::chrono::steady_clock::time_point pendingRuntimeCommandAt{};
        std::string pendingSelectionGroupId;
        std::string pendingSelectionMediaId;
        std::string browsingGroupId;
        std::vector<motion::GroupMetadata> groups;
        std::vector<motion::MediaMetadata> allMedia;
        std::vector<motion::MediaMetadata> filteredMedia;
        std::vector<motion::DisplayTarget> displays;
        std::string selectedDisplayId;
        Microsoft::UI::Dispatching::DispatcherQueueTimer settingsSaveTimer{ nullptr };
        Microsoft::UI::Dispatching::DispatcherQueueTimer catalogSearchTimer{ nullptr };
        Microsoft::UI::Dispatching::DispatcherQueueTimer statusHideTimer{ nullptr };
        Microsoft::UI::Dispatching::DispatcherQueueTimer settingsReloadTimer{ nullptr };
        std::filesystem::file_time_type runtimeWriteTime{};
        std::atomic_bool importing{};
        std::shared_ptr<std::atomic_bool> importCancellation{ std::make_shared<std::atomic_bool>() };
        std::atomic_bool coversRefreshing{};
        std::atomic_bool closing{};
        std::shared_ptr<std::atomic_bool> controllerCancellation{ std::make_shared<std::atomic_bool>() };
        bool controllerStarting{};
        bool initializing{ true };
        bool reorderingGroups{};
        bool optimizationWorkVisible{};
        bool settingsWritable{ true };
        bool mediaLibraryAvailable{ true };
        bool restoreRecoveryBlocked{};
        bool batchSelectionMode{};
        std::optional<motion::MediaLibraryTrustIdentity> mediaLibraryTrust;
        AppPage currentPage{ AppPage::Settings };
        std::unordered_map<std::string, uint8_t> variantSelections;
        std::unordered_map<std::string, std::wstring> mediaGroupNames;
        std::unordered_map<std::string, motion::app::VariantTaskCard> variantTaskCards;
        std::wstring variantViewFingerprint;
        std::wstring originalFailureFingerprint;
        bool originalErrorDialogOpen{};
        std::chrono::steady_clock::time_point originalRecoveryRequestedAt{};
        std::string draggedGroupId;

        void SaveSettings();
        bool TrySaveSettings() noexcept;
        void ReloadExternalSelection();
        void ApplySettingsToControls();
        void LoadGroups();
        void LoadMedia();
        void RefreshMedia();
        std::vector<motion::MediaMetadata> SelectedMediaItems();
        bool CatalogQueryActive();
        void UpdateMediaSelectionVisuals(int32_t selectedIndex);
        void SyncMediaSelectionToApplied();
        void UpdateMediaActionState();
        void UpdatePerformanceModeAvailability();
        void UpdateStatusSummary();
        void UpdateOptimizationProgress();
        void UpdateRuntimeStatus();
        void UpdateOriginalPlaybackWarnings(std::vector<motion::DisplayRuntimeState> const& states);
        void RecoverOriginalPlayback(motion::MediaMetadata const& media, std::string const& mode);
        winrt::fire_and_forget ShowOriginalPlaybackDetails(
            motion::MediaMetadata media, motion::DisplayRuntimeState state);
        void SendRuntimeControl(std::string const& action, std::string const& displayId);
        void LoadDisplayTargets();
        void LoadScenes();
        void UpdateSceneControls();
        void RefreshDisplayLayout();
        void SelectDisplayFromLayout(std::string const& displayId);
        std::pair<std::string, std::string> SelectedWallpaperForTarget() const;
        void SelectWallpaperForTarget(std::string const& groupId, std::string const& mediaId, std::string const& displayId);
        void ReplaceMediaReferences(
            std::string const& oldGroupId, std::string const& oldMediaId,
            std::string const& newGroupId, std::string const& newMediaId);
        void RemoveMediaReferences(std::string const& groupId, std::string const& mediaId);
        void RemoveGroupReferences(std::string const& groupId);
        void ShowSettingsPage();
        void ShowVariantsPage();
        void ShowWallpaperPage();
        void Navigate(AppPage page);
        void RefreshVariants();
        void RequestVariant(motion::MediaMetadata const& media, std::string const& mode);
        void SetVariantPaused(motion::MediaMetadata const& media, bool paused);
        void CancelVariant(motion::MediaMetadata const& media);
        void ConfirmDeleteVariantSelection(motion::MediaMetadata const& media, uint8_t selection);
        winrt::fire_and_forget ImportFiles(std::string kind, std::wstring title, std::wstring pattern);
        winrt::fire_and_forget RefreshMissingCovers(std::string groupId, std::vector<motion::MediaMetadata> media);
        winrt::fire_and_forget MoveMedia(motion::MediaMetadata media, std::string targetGroupId);
        winrt::fire_and_forget DeleteGroup(motion::GroupMetadata group);
        winrt::fire_and_forget DeleteVariantProfiles(motion::MediaMetadata media, uint8_t selection);
        winrt::fire_and_forget DeleteSource(motion::MediaMetadata media);
        winrt::fire_and_forget DeleteMedia(motion::MediaMetadata media);
        winrt::fire_and_forget RepairDuplicates(std::vector<motion::app::DuplicateMediaSet> duplicates);
        winrt::fire_and_forget TrimOptimizationStorage(uint64_t quotaBytes, bool releaseAll);
        winrt::fire_and_forget BackupLibrary(std::filesystem::path destination);
        winrt::fire_and_forget RestoreLibrary(std::filesystem::path backupPath);
        winrt::fire_and_forget MoveLibrary(std::filesystem::path target);
        winrt::fire_and_forget FinalizeLibraryMigration(
            std::shared_ptr<motion::app::LibraryMigrationTransaction> transaction,
            std::shared_ptr<motion::app::AgentLibraryMigrationPause> agentPause,
            std::shared_ptr<motion::app::LibraryMigrationLease> migrationLease,
            std::shared_ptr<std::atomic_bool> cancellation);
        std::shared_ptr<motion::app::LibraryWriteLease> TryAcquireLibraryWrite(bool showError = true);
        void SetLibraryMigrationUi(bool migrating);
        winrt::fire_and_forget StartController();
        void ShowStatus(std::wstring const& message, bool error = false,
            bool persistent = false);
        std::string ActiveGroupId();
        void RenameActiveGroup();
        void ReorderActiveGroup(int direction);
    };
}

namespace winrt::MotionWallpaper::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};
}

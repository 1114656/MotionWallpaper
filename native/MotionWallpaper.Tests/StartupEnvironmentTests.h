#pragma once

#include "../MotionWallpaper.Common/StartupEnvironment.h"

#include <fstream>
#include <thread>

namespace motion::tests
{
    // Register before the ordinary test entry initializes COM/MF. The fixture
    // is this test executable copied into an isolated directory under the exact
    // Agent filename; no installed Motion process is stopped or reconfigured.
    inline std::optional<int> handle_startup_environment_test_command()
    {
        auto executable = startup::process_image(GetCurrentProcess());
        if (!executable || _wcsicmp(executable->filename().c_str(), L"motionwallpaper-agent.exe") != 0) return {};
        std::ifstream modeFile(executable->parent_path() / L"startup-fixture-mode");
        std::string mode;
        modeFile >> mode;
        if (mode == "exit7") return 7;
        if (mode != "wait" && mode != "ready") return {};
        if (mode == "ready") {
            std::ofstream runtime(executable->parent_path() / L"Config" / L"runtime.json");
            runtime << GetCurrentProcessId();
        }
        Sleep(5000); // A failed harness cannot leave a persistent child behind.
        return 0;
    }

    template<typename Require>
    inline void startup_environment_rejects_unsupported_platforms(Require require)
    {
        require(!startup::platform_requirement(10, 19044, IMAGE_FILE_MACHINE_AMD64),
            "Windows 10 before 22H2 was accepted");
        require(static_cast<bool>(startup::platform_requirement(10, 19045, IMAGE_FILE_MACHINE_AMD64)),
            "Windows 10 22H2 x64 was rejected");
        auto emulated = startup::platform_requirement(10, 22631, IMAGE_FILE_MACHINE_ARM64);
        require(emulated && !emulated.warning.empty(), "Windows 11 ARM64 emulation lost its unvalidated warning");
        require(!startup::platform_requirement(10, 19045, IMAGE_FILE_MACHINE_ARM64) &&
            !startup::platform_requirement(10, 19045, IMAGE_FILE_MACHINE_I386),
            "unsupported host architecture was accepted");
    }

    template<typename Require>
    inline void startup_permissions_probe_preserves_files(std::filesystem::path const& root, Require require)
    {
        auto directory = root / L"startup-permissions";
        std::filesystem::create_directories(directory / L"Config");
        auto settings = directory / L"Config" / L"settings.json";
        { std::ofstream file(settings); file << "preserve-exactly"; }
        require(startup::check_data_directory(directory).empty(), "writable startup root was rejected");
        size_t entries{};
        for (auto const& entry : std::filesystem::recursive_directory_iterator(directory)) {
            ++entries;
            require(entry.path().filename().native().find(L".motion-write-check-") == std::wstring::npos,
                "permission probe leaked its temporary file");
        }
        require(entries == 2, "permission probe created a persistent directory or file");
        std::ifstream file(settings);
        std::string content;
        file >> content;
        file.close();
        require(content == "preserve-exactly", "permission probe modified settings contents");
        require(SetFileAttributesW(settings.c_str(), FILE_ATTRIBUTE_READONLY) != FALSE,
            "could not prepare read-only settings fixture");
        auto readOnlyError = startup::check_data_directory(directory);
        SetFileAttributesW(settings.c_str(), FILE_ATTRIBUTE_NORMAL);
        require(!readOnlyError.empty(), "read-only settings reached SettingsStore::Load");
        require(startup::probe_directory_write(settings) != ERROR_SUCCESS,
            "a file used as the data directory passed the write probe");
        require(startup::same_directory(directory, directory / L"Config" / L"..") &&
            !startup::same_directory(directory, root / L"another-install"),
            "installation path normalization accepted another directory or rejected an alias");
    }

    template<typename Require>
    inline void startup_handshake_reports_exit_timeout_and_identity(
        std::filesystem::path const& root, Require require)
    {
        namespace fs = std::filesystem;
        using namespace std::chrono_literals;
        auto directory = root / L"startup-service-fixture";
        fs::create_directories(directory / L"Config");
        auto runtimePath = directory / L"Config" / L"runtime.json";
        auto executable = startup::process_image(GetCurrentProcess());
        require(executable.has_value(), "could not locate test executable");
        fs::copy_file(*executable, directory / L"motionwallpaper-agent.exe", fs::copy_options::overwrite_existing);
        // Keep app-local C++ runtimes available when CI deliberately has no
        // global redistributable. Only adjacent runtime DLLs are copied.
        for (auto const& entry : fs::directory_iterator(executable->parent_path())) {
            auto name = entry.path().filename().wstring();
            if (entry.path().extension() == L".dll" &&
                (name.starts_with(L"vcruntime") || name.starts_with(L"msvcp") || name == L"concrt140.dll")) {
                fs::copy_file(entry.path(), directory / entry.path().filename(), fs::copy_options::overwrite_existing);
            }
        }
        auto isolated = [](fs::path const&) { return startup::InstallationState{}; };
        auto cancelled = [] { return false; };
        auto notReady = [](DWORD) { return false; };
        auto mode = [&](char const* value) { std::ofstream(directory / L"startup-fixture-mode") << value; };
        auto stopFixture = [&](startup::AgentStartResult const& result) {
            if (!result.processId) return;
            unique_handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE | PROCESS_TERMINATE,
                FALSE, result.processId));
            if (!process || WaitForSingleObject(process.get(), 0) != WAIT_TIMEOUT) return;
            auto image = startup::process_image(process.get());
            if (!image || !startup::same_directory(image->parent_path(), directory)) return;
            TerminateProcess(process.get(), 0);
            WaitForSingleObject(process.get(), 2000);
        };

        mode("exit7");
        auto exited = startup::start_or_connect_agent(directory, runtimePath, notReady, cancelled, 2s, isolated);
        stopFixture(exited);
        require(!exited.ready && exited.exitCode == DWORD{ 7 } && !exited.message.empty(),
            "Agent early exit lost its original exit code");

        mode("wait");
        { std::ofstream file(runtimePath); file << "old-runtime"; }
        {
            unique_handle file(CreateFileW(runtimePath.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ |
                FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
            FILETIME ancient{};
            require(file && SetFileTime(file.get(), nullptr, nullptr, &ancient), "could not age stale runtime fixture");
        }
        unsigned handshakeReads{};
        auto started = std::chrono::steady_clock::now();
        auto timedOut = startup::start_or_connect_agent(directory, runtimePath,
            [&](DWORD) { ++handshakeReads; return true; }, cancelled, 200ms, isolated);
        stopFixture(timedOut);
        require(!timedOut.ready && !timedOut.exitCode && !timedOut.message.empty() && handshakeReads == 0 &&
            std::chrono::steady_clock::now() - started < 3s,
            "stale runtime was accepted as readiness or handshake timeout was unbounded");

        unsigned cancelPolls{};
        auto interrupted = startup::start_or_connect_agent(directory, runtimePath, notReady,
            [&] { return ++cancelPolls > 2; }, 2s, isolated);
        stopFixture(interrupted);
        require(interrupted.cancelled && !interrupted.ready, "closing App did not cancel its startup wait");

        mode("ready");
        auto ready = startup::start_or_connect_agent(directory, runtimePath, [&](DWORD processId) {
            DWORD runtimeId{};
            std::ifstream(runtimePath) >> runtimeId;
            return runtimeId == processId;
        }, cancelled, 2s, isolated);
        stopFixture(ready);
        require(ready.ready && ready.processId != 0, "matching live Agent runtime did not complete the handshake");

        auto conflict = startup::start_or_connect_agent(directory, runtimePath, notReady, cancelled, 2s,
            [](fs::path const&) { startup::InstallationState state; state.conflict = L"another installation"; return state; });
        require(!conflict.ready && !conflict.processId && conflict.message == L"another installation",
            "cross-install conflict launched a new Agent");
        auto missing = startup::start_or_connect_agent(directory / L"missing", runtimePath, notReady, cancelled, 2s, isolated);
        require(!missing.ready && missing.systemError && !missing.message.empty(),
            "CreateProcess failure was silently discarded");
    }
}

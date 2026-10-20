// CloudBlock Windows native WNBD adapter.
// WNBD headers supplied by a separately obtained, pinned Cloudbase WNBD SDK.
// libwnbd.dll is loaded from an explicit path; this program does not install drivers.
// All I/O is serialized: Store is intentionally not thread-safe yet.
#ifdef _WIN32
#define NOMINMAX
#include "block_store.hpp"
#include <wnbd.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>

namespace {
constexpr std::uint32_t BLOCK_SIZE = 512;
constexpr char INSTANCE[] = "CloudBlockLocal";
std::atomic_bool shutdown_requested{false};

struct WnbdApi {
    HMODULE handle = nullptr;
    decltype(&WnbdCreate) create = nullptr;
    decltype(&WnbdStartDispatcher) start_dispatcher = nullptr;
    decltype(&WnbdWaitDispatcher) wait_dispatcher = nullptr;
    decltype(&WnbdRemove) remove = nullptr;
    decltype(&WnbdClose) close = nullptr;
    decltype(&WnbdGetUserContext) get_context = nullptr;
    decltype(&WnbdSendResponse) send_response = nullptr;

    template<class T>
    T load(const char* name) {
        auto p = GetProcAddress(handle, name);
        if (!p) throw std::runtime_error(std::string("Missing libwnbd export: ") + name);
        return reinterpret_cast<T>(p);
    }

    explicit WnbdApi(const fs::path& dll) {
        check(dll.is_absolute() && dll.filename() == L"libwnbd.dll",
              "Use an absolute path to a verified libwnbd.dll");
        check(fs::exists(dll), "libwnbd.dll not found");
        // Never load a DLL from PATH or the current working directory implicitly.
        handle = LoadLibraryExW(dll.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!handle) throw std::runtime_error(
            "LoadLibraryExW(libwnbd.dll) failed: " + std::to_string(GetLastError()));
        try {
            create = load<decltype(create)>("WnbdCreate");
            start_dispatcher = load<decltype(start_dispatcher)>("WnbdStartDispatcher");
            wait_dispatcher = load<decltype(wait_dispatcher)>("WnbdWaitDispatcher");
            remove = load<decltype(remove)>("WnbdRemove");
            close = load<decltype(close)>("WnbdClose");
            get_context = load<decltype(get_context)>("WnbdGetUserContext");
            send_response = load<decltype(send_response)>("WnbdSendResponse");
        } catch (...) {
            FreeLibrary(handle);
            handle = nullptr;
            throw;
        }
    }
    ~WnbdApi() { if (handle) FreeLibrary(handle); }
    WnbdApi(const WnbdApi&) = delete;
    WnbdApi& operator=(const WnbdApi&) = delete;
};

struct Engine {
    Store store;
    const fs::path root;
    std::mutex mu;
    WnbdApi& api;
    explicit Engine(const fs::path& location, WnbdApi& a)
        : store(location, CAPACITY), root(location), api(a) {}
    void ensure_host_reserve() const {
        // All backing and cache allocations share E:. Leave a minimum reserve.
        const auto available = fs::space(root).available;
        check(available >= 8ULL * 1024 * MIB + 64ULL * MIB,
              "host disk E: free-space guard triggered (8 GiB reserve)");
    }
};

WnbdApi* active_api = nullptr;

Engine* context(PWNBD_DISK disk) noexcept {
    if (!active_api) return nullptr;
    void* c = nullptr;
    if (active_api->get_context(disk, &c) != 0) return nullptr;
    return static_cast<Engine*>(c);
}

void respond(PWNBD_DISK disk, UINT64 request, WnbdRequestType op,
             bool succeeded, void* data = nullptr, UINT32 length = 0) noexcept {
    if (!active_api) return;
    WNBD_IO_RESPONSE response{};
    response.RequestHandle = request;
    response.RequestType = op;
    if (!succeeded) {
        response.Status.ScsiStatus = 2; // CHECK_CONDITION
        response.Status.SenseKey = 4;   // HARDWARE_ERROR (generic backing failure)
        response.Status.ASC = 0x44;     // INTERNAL_TARGET_FAILURE
    }
    DWORD err = active_api->send_response(
        disk, &response, succeeded ? data : nullptr, succeeded ? length : 0);
    if (err) {
        std::cerr << "WnbdSendResponse(" << request << ") failed: " << err << "\n";
    }
}

bool offset_size(UINT64 block_address, UINT32 count, u64& offset, std::size_t& bytes) {
    if (block_address > VOLUME / BLOCK_SIZE ||
        static_cast<u64>(count) > VOLUME / BLOCK_SIZE - block_address ||
        static_cast<u64>(count) * BLOCK_SIZE > WNBD_DEFAULT_MAX_TRANSFER_LENGTH)
        return false;
    offset = block_address * BLOCK_SIZE;
    bytes = static_cast<std::size_t>(count) * BLOCK_SIZE;
    return true;
}

void on_read(PWNBD_DISK disk, UINT64 request, PVOID buffer,
             UINT64 lba, UINT32 count, BOOLEAN /*fua*/) noexcept {
    Engine* engine = context(disk);
    u64 offset = 0;
    std::size_t bytes = 0;
    bool ok = false;
    if (engine && offset_size(lba, count, offset, bytes) && (!bytes || buffer)) {
        try {
            std::lock_guard<std::mutex> lock(engine->mu);
            engine->ensure_host_reserve();
            engine->store.read(offset, static_cast<char*>(buffer), bytes);
            ok = true;
        } catch (const std::exception& e) {
            std::cerr << "WNBD READ: " << e.what() << "\n";
        }
    }
    respond(disk, request, WnbdReqTypeRead, ok, buffer, static_cast<UINT32>(bytes));
}

void on_write(PWNBD_DISK disk, UINT64 request, PVOID buffer,
              UINT64 lba, UINT32 count, BOOLEAN fua) noexcept {
    Engine* engine = context(disk);
    u64 offset = 0;
    std::size_t bytes = 0;
    bool ok = false;
    if (engine && offset_size(lba, count, offset, bytes) && (!bytes || buffer)) {
        try {
            std::lock_guard<std::mutex> lock(engine->mu);
            engine->ensure_host_reserve();
            engine->store.write(offset, static_cast<const char*>(buffer), bytes);
            if (fua) engine->store.flush();
            ok = true;
        } catch (const std::exception& e) {
            std::cerr << "WNBD WRITE: " << e.what() << "\n";
        }
    }
    respond(disk, request, WnbdReqTypeWrite, ok);
}

void on_flush(PWNBD_DISK disk, UINT64 request, UINT64, UINT32) noexcept {
    Engine* engine = context(disk);
    bool ok = false;
    if (engine) {
        try {
            std::lock_guard<std::mutex> lock(engine->mu);
            engine->ensure_host_reserve();
            engine->store.flush();
            ok = true;
        } catch (const std::exception& e) {
            std::cerr << "WNBD FLUSH: " << e.what() << "\n";
        }
    }
    respond(disk, request, WnbdReqTypeFlush, ok);
}

void on_unmap(PWNBD_DISK disk, UINT64 request, PWNBD_UNMAP_DESCRIPTOR, UINT32) noexcept {
    // UnmapSupported is deliberately false. Never drop chunks yet.
    respond(disk, request, WnbdReqTypeUnmap, false);
}

BOOL WINAPI console_handler(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT) {
        shutdown_requested.store(true);
        return TRUE;
    }
    return FALSE;
}

void verify_root(const fs::path& root) {
    check(root.is_absolute(), "storage root must be absolute");
    check(root.root_name().wstring() == L"E:", "prototype restricted to E: local storage drive");
    check(root.lexically_normal() == fs::path(LR"(E:\CloudBlock)"),
          "prototype must use E:\\CloudBlock (never X: or another mounted drive)");
    check(fs::is_directory(root / "disk") && fs::is_directory(root / "cache") &&
          fs::is_directory(root / "metadata") && fs::is_directory(root / "logs"),
          "initialize E:\\CloudBlock directories first with scripts\\windows\\prepare.ps1");
    const auto capacity = fs::space(root).available;
    check(capacity >= 8ULL * 1024 * MIB,
          "less than 8 GiB free on host E:, refusing to map");
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc != 3 || std::wstring(argv[1]) != L"--wnbd-dll") {
            std::wcerr << L"Usage: cloudblock-windows --wnbd-dll \"C:\\absolute\\path\\libwnbd.dll\"\n"
                          L"Storage root: E:\\CloudBlock; disk name: CloudBlockLocal\n";
            return 2;
        }

        const fs::path root(LR"(E:\CloudBlock)");
        verify_root(root);
        SetConsoleCtrlHandler(console_handler, TRUE);
        WnbdApi api(argv[2]);
        Engine engine(root, api);
        active_api = &api;

        WNBD_PROPERTIES props{};
        // WNBD library expects zero-initialized defaults for non-NBD connection.
        std::strncpy(props.InstanceName, INSTANCE, sizeof(props.InstanceName) - 1);
        std::strncpy(props.SerialNumber, "CLOUDBLOCK-LOCAL-001",
                     sizeof(props.SerialNumber) - 1);
        std::strncpy(props.Owner, "CloudBlock", sizeof(props.Owner) - 1);
        props.BlockSize = BLOCK_SIZE;
        props.BlockCount = VOLUME / BLOCK_SIZE;
        props.Flags.FlushSupported = 1;
        props.Flags.FUASupported = 1;
        props.Flags.UnmapSupported = 0;   // TRIM not implemented
        props.Flags.ReadOnly = 0;
        props.Flags.UseUserspaceNbd = 0;  // use direct user-mode callback interface

        WNBD_INTERFACE callbacks{};
        callbacks.Read = on_read;
        callbacks.Write = on_write;
        callbacks.Flush = on_flush;
        callbacks.Unmap = on_unmap;

        PWNBD_DISK disk = nullptr;
        auto status = api.create(&props, &callbacks, &engine, &disk);
        if (status || !disk)
            throw std::runtime_error("WnbdCreate failed: " + std::to_string(status));
        std::cout << "Mapped WNBD CloudBlockLocal, capacity=64GiB, cache=1GiB"
                     "\nStorage E:\\CloudBlock; Ctrl+C for graceful unmap\n";
        status = api.start_dispatcher(disk, 1);
        if (status) {
            // Cleanup mapping even if dispatcher startup fails.
            api.remove(disk, nullptr);
            api.close(disk);
            throw std::runtime_error("WnbdStartDispatcher failed: " + std::to_string(status));
        }
        while (!shutdown_requested.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(200));

        std::cout << "Requesting soft removal; close X: files before stopping...\n";
        // NEVER request hard removal; driver will coordinate outstanding requests.
        auto remove_status = api.remove(disk, nullptr);
        if (remove_status) {
            std::cerr << "WnbdRemove returned " << remove_status
                      << " (device may remain connected; operator inspection required)\n";
            // Do not free engine while driver may still call callbacks.
            return 1;
        }
        auto wait_status = api.wait_dispatcher(disk);
        if (wait_status) {
            std::cerr << "WnbdWaitDispatcher returned " << wait_status << "\n";
            return 1;
        }
        api.close(disk);
        active_api = nullptr;
        std::cout << "WNBD disk gracefully unmapped\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Windows CloudBlock error: " << e.what() << "\n";
        return 1;
    }
}
#else
int main() { return 1; }
#endif

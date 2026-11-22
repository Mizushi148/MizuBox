// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Clean-room Windows x64 implementation of Xbox GDK Runtime APIs (xgameruntime.dll):
// - Configuration (wsx.ini + WSX_* overrides) & Structured API Call Tracer (WSX_TRACE=1)
// - XGameRuntimeInitialize / XGameRuntimeUninitialize
// - XSystem (DeviceType, AnalyticsInfo, RuntimeInfo, ConsoleId, AppSpecificDeviceId, SandboxId, IsHandleValid)
// - XThread (TimeSensitive TLS flag)
// - XTaskQueue (Manual, ThreadPool, SerializedThreadPool, Immediate, ThreadPool Timers, real Terminate wait)
// - XAsync & XAsyncProvider
// - XUser (Gamertag, XUID, LocalId, State, Privileges, Change Events, clean uninit teardown)
// - XGameSave & XGameSaveFiles (Atomic .tmp->rename disk-backed Connected Storage, real disk/quota calculation)
// - XNetworking (ConnectivityHint, Change Events, Preferred UDP Multiplayer Port)
// - XPersistentLocalStorage (Real local cache folder & real disk space accounting)
// - XPackage (MicrosoftGame.Config-aware package identity, locale, enumeration, installation monitor, content mount)

#include "wsx_config.h"
#include "wsx_package_config.h"
#include "include/xbox/XGameRuntimeInit.h"
#include "include/xbox/XSystem.h"
#include "include/xbox/XThread.h"
#include "include/xbox/appnotify.h"
#include "include/xbox/XTaskQueue.h"
#include "include/xbox/XAsync.h"
#include "include/xbox/XAsyncProvider.h"
#include "include/xbox/XUser.h"
#include "include/xbox/XGameSave.h"
#include "include/xbox/XGameSaveFiles.h"
#include "include/xbox/XNetworking.h"
#include "include/xbox/XPersistentLocalStorage.h"
#include "include/xbox/XPackage.h"
#include "include/xbox/XStore.h"
#include "include/xbox/XGameStreaming.h"
#include "include/xbox/xsapi_c.h"

#include <bcrypt.h>
#include <objbase.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr const char* kMod = "xgameruntime";

// ============================================================================
// PLM AppState Notifications & Runtime Lifetime
// ============================================================================

struct AppStateNode {
    PAPPSTATE_CHANGE_ROUTINE routine = nullptr;
    PVOID context = nullptr;
};

struct AppConstrainNode {
    PAPPCONSTRAIN_CHANGE_ROUTINE routine = nullptr;
    PVOID context = nullptr;
};

std::atomic<int> g_init_count{0};
thread_local bool g_thread_time_sensitive = false;

// ============================================================================
// XTaskQueue Implementation (ThreadPool Timers + Wait-Safe Terminate)
// ============================================================================

struct TaskQueueItem {
    XTaskQueueCallback* callback = nullptr;
    void* context = nullptr;
};

struct PortQueue {
    XTaskQueueDispatchMode mode = XTaskQueueDispatchMode::ThreadPool;
    std::mutex mtx;
    std::condition_variable cv;
    std::deque<TaskQueueItem> items;
    bool serializing = false;
};

struct DelayedTimerNode {
    PTP_TIMER timer = nullptr;
    XTaskQueueHandle queue = nullptr;
    XTaskQueuePort port = XTaskQueuePort::Work;
    void* context = nullptr;
    XTaskQueueCallback* callback = nullptr;
    std::atomic<bool> fired{false};
};

struct TaskQueueMonitorReg {
    uint64_t token = 0;
    void* context = nullptr;
    XTaskQueueMonitorCallback* callback = nullptr;
};

} // namespace

struct XTaskQueuePortObject {
    XTaskQueueHandle owner = nullptr;
    XTaskQueuePort port = XTaskQueuePort::Work;
};

struct XTaskQueueObject {
    std::atomic<int> refCount{1};
    std::atomic<bool> terminated{false};
    std::atomic<int> activeWorkers{0};
    std::mutex idleMtx;
    std::condition_variable idleCv;
    std::mutex timerMtx;
    std::vector<DelayedTimerNode*> pendingTimers;
    PortQueue workPort;
    PortQueue completionPort;
    // Optional composite delegation targets (retains target queue)
    XTaskQueueHandle workDelegateQueue = nullptr;
    XTaskQueuePort workDelegatePort = XTaskQueuePort::Work;
    XTaskQueueHandle completionDelegateQueue = nullptr;
    XTaskQueuePort completionDelegatePort = XTaskQueuePort::Completion;
    std::mutex monitorMtx;
    std::vector<TaskQueueMonitorReg> monitors;
    XTaskQueuePortObject workPortHandleObj{this, XTaskQueuePort::Work};
    XTaskQueuePortObject completionPortHandleObj{this, XTaskQueuePort::Completion};
};

namespace {

std::mutex g_process_queue_mtx;
XTaskQueueHandle g_process_queue = nullptr;

void RetainQueue(XTaskQueueHandle q) noexcept {
    if (q) {
        q->refCount.fetch_add(1, std::memory_order_relaxed);
    }
}

void ReleaseQueue(XTaskQueueHandle q) noexcept {
    if (q && q->refCount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        if (q->workDelegateQueue) ReleaseQueue(q->workDelegateQueue);
        if (q->completionDelegateQueue) ReleaseQueue(q->completionDelegateQueue);
        delete q;
    }
}

void ResolveEffectivePort(XTaskQueueHandle& q, XTaskQueuePort& port) noexcept {
    for (int depth = 0; depth < 8 && q; ++depth) {
        if (port == XTaskQueuePort::Work && q->workDelegateQueue) {
            port = q->workDelegatePort;
            q = q->workDelegateQueue;
        } else if (port == XTaskQueuePort::Completion && q->completionDelegateQueue) {
            port = q->completionDelegatePort;
            q = q->completionDelegateQueue;
        } else {
            break;
        }
    }
}

void NotifyQueueMonitors(XTaskQueueHandle q, XTaskQueuePort port) noexcept {
    if (!q) return;
    std::vector<TaskQueueMonitorReg> snapshot;
    {
        std::lock_guard<std::mutex> lk(q->monitorMtx);
        snapshot = q->monitors;
    }
    for (const auto& m : snapshot) {
        if (m.callback) {
            m.callback(m.context, q, port);
        }
    }
}

XTaskQueueHandle GetOrCreateDefaultProcessQueue() noexcept {
    std::lock_guard<std::mutex> lock(g_process_queue_mtx);
    if (!g_process_queue) {
        auto* q = new (std::nothrow) XTaskQueueObject();
        if (q) {
            q->workPort.mode = XTaskQueueDispatchMode::ThreadPool;
            q->completionPort.mode = XTaskQueueDispatchMode::ThreadPool;
            g_process_queue = q;
        }
    }
    return g_process_queue;
}

PortQueue& GetPort(XTaskQueueHandle q, XTaskQueuePort port) noexcept {
    return (port == XTaskQueuePort::Work) ? q->workPort : q->completionPort;
}

bool DispatchOneFromPort(XTaskQueueHandle q, XTaskQueuePort port, uint32_t timeoutInMs, bool canceled = false) noexcept {
    if (!q) return false;
    ResolveEffectivePort(q, port);
    if (!q) return false;
    PortQueue& pq = GetPort(q, port);
    TaskQueueItem item{};
    {
        std::unique_lock<std::mutex> lock(pq.mtx);
        if (pq.items.empty()) {
            if (timeoutInMs == 0 || q->terminated.load(std::memory_order_relaxed)) {
                return false;
            }
            pq.cv.wait_for(lock, std::chrono::milliseconds(timeoutInMs), [&] {
                return !pq.items.empty() || q->terminated.load(std::memory_order_relaxed);
            });
            if (pq.items.empty()) {
                return false;
            }
        }
        item = pq.items.front();
        pq.items.pop_front();
    }
    if (item.callback) {
        item.callback(item.context, canceled || q->terminated.load(std::memory_order_relaxed));
    }
    return true;
}

struct ThreadPoolDispatchArgs {
    XTaskQueueHandle queue;
    XTaskQueuePort port;
};

VOID CALLBACK ThreadPoolPortWorker(PTP_CALLBACK_INSTANCE, PVOID context) noexcept {
    auto* args = static_cast<ThreadPoolDispatchArgs*>(context);
    if (!args) return;
    XTaskQueueHandle q = args->queue;
    XTaskQueuePort port = args->port;
    delete args;

    PortQueue& pq = GetPort(q, port);
    if (pq.mode == XTaskQueueDispatchMode::SerializedThreadPool) {
        bool acquired = false;
        {
            std::lock_guard<std::mutex> lock(pq.mtx);
            if (!pq.serializing) {
                pq.serializing = true;
                acquired = true;
            }
        }
        if (acquired) {
            while (DispatchOneFromPort(q, port, 0)) {}
            std::lock_guard<std::mutex> lock(pq.mtx);
            pq.serializing = false;
        }
    } else {
        DispatchOneFromPort(q, port, 0);
    }

    if (q->activeWorkers.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        std::lock_guard<std::mutex> lock(q->idleMtx);
        q->idleCv.notify_all();
    }
    ReleaseQueue(q);
}

VOID CALLBACK DelayedTimerCallback(PTP_CALLBACK_INSTANCE, PVOID context, PTP_TIMER timer) noexcept {
    auto* node = static_cast<DelayedTimerNode*>(context);
    if (!node) return;
    XTaskQueueHandle q = node->queue;

    bool should_close = false;
    {
        std::lock_guard<std::mutex> lock(q->timerMtx);
        auto it = std::find(q->pendingTimers.begin(), q->pendingTimers.end(), node);
        if (it != q->pendingTimers.end()) {
            q->pendingTimers.erase(it);
            should_close = true;
        }
    }

    if (!node->fired.exchange(true, std::memory_order_acq_rel)) {
        if (q->terminated.load(std::memory_order_acquire)) {
            if (node->callback) {
                node->callback(node->context, true);
            }
        } else {
            XTaskQueueSubmitCallback(q, node->port, node->context, node->callback);
        }
    }

    if (should_close) {
        CloseThreadpoolTimer(timer);
        delete node;
    }

    if (q->activeWorkers.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        std::lock_guard<std::mutex> lock(q->idleMtx);
        q->idleCv.notify_all();
    }
    ReleaseQueue(q);
}

// ============================================================================
// XAsync & XAsyncProvider Implementation
// ============================================================================

constexpr uint64_t kWsxAsyncMagic = 0x5753584153594E43ULL; // "WSXASYNC"

struct WsxAsyncState {
    XAsyncBlock* asyncBlock = nullptr;
    XTaskQueueHandle queue = nullptr;
    void* providerContext = nullptr;
    bool ownsProviderContext = false;
    const void* identity = nullptr;
    const char* identityName = nullptr;
    XAsyncProvider* provider = nullptr;

    std::mutex mtx;
    std::condition_variable cv;
    std::atomic<HRESULT> status{E_PENDING};
    std::atomic<size_t> requiredBufferSize{0};
    std::atomic<bool> completing{false};
    std::atomic<bool> workDone{false};
    std::atomic<bool> cleanedUp{false};
    std::atomic<bool> completionFired{false};
    std::atomic<bool> completionDone{false};
    std::atomic<DWORD> completionThreadId{0};
    std::atomic<bool> resultRetrieved{false};
    std::atomic<bool> canceled{false};
};

struct AsyncInternalLayout {
    uint64_t magic;
    WsxAsyncState* state;
    uint64_t reserved[2];
};

static_assert(sizeof(AsyncInternalLayout) <= sizeof(XAsyncBlock::internal),
              "AsyncInternalLayout must fit in XAsyncBlock::internal");

WsxAsyncState* GetAsyncState(XAsyncBlock* asyncBlock) noexcept {
    if (!asyncBlock) return nullptr;
    auto* layout = reinterpret_cast<AsyncInternalLayout*>(asyncBlock->internal);
    if (layout->magic != kWsxAsyncMagic) return nullptr;
    return layout->state;
}

void CleanupAsyncProviderOnce(WsxAsyncState* state) noexcept {
    if (!state) return;
    if (!state->cleanedUp.exchange(true, std::memory_order_acq_rel)) {
        if (state->provider) {
            XAsyncProviderData data{};
            data.async = state->asyncBlock;
            data.context = state->providerContext;
            state->provider(XAsyncOp::Cleanup, &data);
        }
        if (state->ownsProviderContext && state->providerContext) {
            std::free(state->providerContext);
            state->providerContext = nullptr;
        }
    }
}

void FireAsyncCompletionIfNeeded(WsxAsyncState* state) noexcept {
    if (!state) return;
    if (!state->completionFired.exchange(true, std::memory_order_acq_rel)) {
        state->completionThreadId.store(GetCurrentThreadId(), std::memory_order_release);
        // When no result buffer is expected (or the op failed), run XAsyncOp::Cleanup
        // before invoking asyncBlock->callback (which is allowed to free asyncBlock).
        const HRESULT st = state->status.load(std::memory_order_acquire);
        const size_t reqSize = state->requiredBufferSize.load(std::memory_order_acquire);
        if (reqSize == 0 || FAILED(st)) {
            CleanupAsyncProviderOnce(state);
        }
        XAsyncBlock* ab = state->asyncBlock;
        if (ab && ab->callback) {
            ab->callback(ab);
        }
        {
            std::lock_guard<std::mutex> lock(state->mtx);
            state->completionDone.store(true, std::memory_order_release);
            state->cv.notify_all();
        }
    }
}

void CALLBACK AsyncCompletionCallback(void* context, bool /*canceled*/) noexcept {
    auto* state = static_cast<WsxAsyncState*>(context);
    FireAsyncCompletionIfNeeded(state);
}

void CALLBACK AsyncWorkCallback(void* context, bool canceled) noexcept {
    auto* state = static_cast<WsxAsyncState*>(context);
    if (!state) return;

    if (canceled || state->canceled.load(std::memory_order_relaxed)) {
        XAsyncComplete(state->asyncBlock, E_ABORT, 0);
        return;
    }

    if (state->provider) {
        XAsyncProviderData data{};
        data.async = state->asyncBlock;
        data.context = state->providerContext;
        const HRESULT hr = state->provider(XAsyncOp::DoWork, &data);
        // E_PENDING from XAsyncOp::DoWork means the provider is still in flight
        // (it either rescheduled itself via XAsyncSchedule or awaits an external callback).
        if (hr == E_PENDING) {
            return;
        }
        if (FAILED(hr) && !state->workDone.load(std::memory_order_acquire)) {
            XAsyncComplete(state->asyncBlock, hr, 0);
        } else if (hr == S_OK && !state->workDone.load(std::memory_order_acquire)) {
            XAsyncComplete(state->asyncBlock, S_OK, 0);
        }
    } else {
        XAsyncComplete(state->asyncBlock, S_OK, 0);
    }
}

// ============================================================================
// XUser State & Lifecycle
// ============================================================================

} // namespace

struct XUser {
    std::atomic<int> refCount{1};
    uint64_t xuid = 0x0009000000000001ULL;
    XUserLocalId localId{1ULL};
    XUserState state = XUserState::SignedIn;
    std::string modernGamertag = "LocalPlayer#0001";
    std::string classicGamertag = "LocalPlayer";
    std::string suffix = "7777";
};

namespace {

std::mutex g_user_mtx;
XUser* g_default_user = nullptr;
std::atomic<uint64_t> g_next_token{1};

struct UserChangeReg {
    uint64_t token = 0;
    XTaskQueueHandle queue = nullptr;
    void* context = nullptr;
    XUserChangeEventCallback* callback = nullptr;
};
std::vector<UserChangeReg> g_user_change_regs;

XUser* GetOrCreateDefaultUser() noexcept {
    std::lock_guard<std::mutex> lock(g_user_mtx);
    if (!g_default_user) {
        auto* u = new (std::nothrow) XUser();
        if (!u) return nullptr;

        const std::string gt = wsx::get_config_string("WSX_GAMERTAG", "User", "gamertag", "LocalPlayer#0001");
        if (!gt.empty()) {
            u->modernGamertag = gt;
            const auto hashPos = u->modernGamertag.find('#');
            if (hashPos != std::string::npos) {
                u->classicGamertag = u->modernGamertag.substr(0, std::min<size_t>(hashPos, 15));
                u->suffix = u->modernGamertag.substr(hashPos + 1);
            } else {
                u->classicGamertag = u->modernGamertag.substr(0, 15);
                u->suffix = "";
            }
        }
        u->xuid = wsx::get_config_uint64("WSX_XUID", "User", "xuid", 0x0009000000000001ULL);
        g_default_user = u;
    }
    g_default_user->refCount.fetch_add(1, std::memory_order_relaxed);
    return g_default_user;
}

void CleanupDefaultUserLocked() noexcept {
    if (g_default_user) {
        if (g_default_user->refCount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            delete g_default_user;
        }
        g_default_user = nullptr;
    }
}

bool IsValidUser(XUserHandle user) noexcept {
    return user != nullptr && user->refCount.load(std::memory_order_relaxed) > 0;
}

// ============================================================================
// XNetworking State
// ============================================================================

struct NetChangeReg {
    uint64_t token = 0;
    XTaskQueueHandle queue = nullptr;
    void* context = nullptr;
    XNetworkingConnectivityHintChangedCallback* callback = nullptr;
};
struct NetUdpPortReg {
    uint64_t token = 0;
    XTaskQueueHandle queue = nullptr;
    void* context = nullptr;
    XNetworkingPreferredLocalUdpMultiplayerPortChangedCallback* callback = nullptr;
};
std::mutex g_net_mtx;
std::vector<NetChangeReg> g_net_change_regs;
std::vector<NetUdpPortReg> g_net_udp_port_regs;

bool IsOfflineNetModeEnabled() noexcept {
    wchar_t buf[16]{};
    const DWORD n = GetEnvironmentVariableW(L"WSX_OFFLINE_NET", buf, 16);
    if (n == 0 || n >= 16) return false;
    return (buf[0] == L'1' || buf[0] == L't' || buf[0] == L'T' || buf[0] == L'y' || buf[0] == L'Y');
}

// ============================================================================
// XGameSave & Disk Helpers (Atomic Writes + Real Quota)
// ============================================================================

} // namespace

struct XGameSaveProvider {
    uint64_t xuid = 0;
    std::string scid;
    fs::path rootDir;
    int64_t maxQuotaBytes = 256LL * 1024LL * 1024LL;
};

struct XGameSaveContainer {
    XGameSaveProviderHandle provider = nullptr;
    std::string name;
    fs::path containerDir;
};

struct PendingBlobOp {
    bool isDelete = false;
    std::string blobName;
    std::vector<uint8_t> data;
};

struct XGameSaveUpdate {
    XGameSaveContainerHandle container = nullptr;
    std::string displayName;
    std::vector<PendingBlobOp> ops;
};

struct XPackageMount {
    std::string packageId;
    std::string mountPath;
};

struct XPackageInstallationMonitor {
    std::string packageId;
    uint64_t totalBytes = 0;
};

namespace {

fs::path Utf8ToPath(const std::string& utf8) {
    if (utf8.empty()) return {};
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (wlen <= 1) return fs::path(utf8);
    std::wstring ws(static_cast<size_t>(wlen - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, ws.data(), wlen);
    return fs::path(ws);
}

fs::path GetBaseSaveDirectory() {
    const std::string cfg = wsx::get_config_string("WSX_GAMESAVE_DIR", "Storage", "gamesave_dir", "");
    if (!cfg.empty()) {
        return Utf8ToPath(cfg);
    }
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    return fs::path(exePath).parent_path() / L"xgs_saves";
}

fs::path GetPlsDirectory() {
    const std::string cfg = wsx::get_config_string("WSX_PLS_DIR", "Storage", "pls_dir", "");
    fs::path dir;
    if (!cfg.empty()) {
        dir = Utf8ToPath(cfg);
    } else {
        wchar_t exePath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        dir = fs::path(exePath).parent_path() / L"pls_cache";
    }
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

uint64_t ComputeDirectoryUsedBytes(const fs::path& dir, const char* ignoreFile = nullptr) noexcept {
    std::error_code ec;
    if (!fs::exists(dir, ec)) return 0;
    uint64_t total = 0;
    for (const auto& entry : fs::recursive_directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        if (ignoreFile && entry.path().filename().string() == ignoreFile) continue;
        total += entry.file_size(ec);
    }
    return total;
}

uint64_t QueryDiskFreeBytes(const fs::path& dir) noexcept {
    ULARGE_INTEGER freeAvail{}, totalBytes{}, totalFree{};
    std::wstring wdir = dir.wstring();
    if (GetDiskFreeSpaceExW(wdir.c_str(), &freeAvail, &totalBytes, &totalFree)) {
        return freeAvail.QuadPart;
    }
    return 1024ULL * 1024ULL * 1024ULL; // 1 GB safe fallback
}

bool AtomicWriteBinaryFile(const fs::path& targetPath, const uint8_t* data, size_t size) noexcept {
    const DWORD tid = GetCurrentThreadId();
    static std::atomic<uint64_t> tmpSeq{1};
    wchar_t tmpSuffix[64]{};
    std::swprintf(tmpSuffix, std::size(tmpSuffix), L".wsx_tmp.%lu.%llu",
                  static_cast<unsigned long>(tid),
                  static_cast<unsigned long long>(tmpSeq.fetch_add(1, std::memory_order_relaxed)));
    fs::path tmpPath = targetPath;
    tmpPath += tmpSuffix;

    {
        std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        if (data && size > 0) {
            out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
            if (!out.good()) {
                out.close();
                DeleteFileW(tmpPath.c_str());
                return false;
            }
        }
        out.flush();
    }

    if (!MoveFileExW(tmpPath.c_str(), targetPath.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmpPath.c_str());
        return false;
    }
    return true;
}

std::string SanitizePathComponent(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (char c : raw) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.') {
            out.push_back(c);
        } else {
            out.push_back('_');
        }
    }
    if (out.empty()) out = "default";
    return out;
}

constexpr const char* kMetaFileName = ".wsx_container_meta";

std::string ReadContainerDisplayName(const fs::path& containerDir, const std::string& fallback) {
    std::ifstream in(containerDir / kMetaFileName, std::ios::binary);
    if (!in) return fallback;
    std::string line;
    std::getline(in, line);
    return line.empty() ? fallback : line;
}

void WriteContainerDisplayName(const fs::path& containerDir, const std::string& displayName) {
    if (displayName.empty()) return;
    AtomicWriteBinaryFile(containerDir / kMetaFileName,
                          reinterpret_cast<const uint8_t*>(displayName.data()),
                          displayName.size());
}

HRESULT CopyStringToBuffer(const std::string& src, size_t bufSize, char* buf, size_t* used) noexcept {
    const size_t needed = src.size() + 1;
    if (used) *used = needed;
    if (!buf || bufSize < needed) {
        return E_NOT_SUFFICIENT_BUFFER;
    }
    std::memcpy(buf, src.c_str(), needed);
    return S_OK;
}

struct ParsedGameIdentity {
    std::string packageId = "MizuBox.Title_1.0.0.0_x64";
    std::string displayName = "MizuBox Title";
    std::string publisher = "Mizushi (MizuBox)";
    std::string titleId = "00007777";
    std::string storeId = "9NBLGGH4WSX0";
    XVersion version{1, 0, 0, 0};
};

ParsedGameIdentity ReadLocalGameConfigIdentity() {
    const auto& cfg = wsx_pkg::get_hydrated_config();
    ParsedGameIdentity id;
    id.packageId = cfg.package_full_name.substr(0, XPACKAGE_IDENTIFIER_MAX_LENGTH - 1);
    id.displayName = cfg.display_name;
    id.publisher = cfg.publisher_display_name;
    id.titleId = cfg.title_id;
    id.storeId = cfg.store_id;
    id.version = XVersion{cfg.ver_major, cfg.ver_minor, cfg.ver_build, cfg.ver_rev};
    return id;
}

} // namespace

// ============================================================================
// Exported C APIs
// ============================================================================

extern "C" {

HRESULT WINAPI XGameRuntimeInitialize() noexcept {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const int count = g_init_count.fetch_add(1, std::memory_order_acq_rel) + 1;
    return wsx::trace_hr(kMod, "XGameRuntimeInitialize", S_OK, "refCount=%d", count);
}

VOID WINAPI XGameRuntimeUninitialize() noexcept {
    const int prev = g_init_count.fetch_sub(1, std::memory_order_acq_rel);
    if (prev <= 1) {
        g_init_count.store(0, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(g_user_mtx);
            CleanupDefaultUserLocked();
            g_user_change_regs.clear();
        }
        {
            std::lock_guard<std::mutex> lock(g_net_mtx);
            g_net_change_regs.clear();
            g_net_udp_port_regs.clear();
        }
        {
            std::lock_guard<std::mutex> lock(g_process_queue_mtx);
            if (g_process_queue) {
                ReleaseQueue(g_process_queue);
                g_process_queue = nullptr;
            }
        }
    }
    wsx::trace_void(kMod, "XGameRuntimeUninitialize", "prevRef=%d", prev);
}

// ----------------------------------------------------------------------------
// XSystem Exports
// ----------------------------------------------------------------------------

XSystemDeviceType WINAPI XSystemGetDeviceType() noexcept {
    const std::string val = wsx::detail::to_lower_ascii(
        wsx::get_config_string("WSX_CONSOLE_TYPE", "Console", "type", "anaconda"));
    XSystemDeviceType dt = XSystemDeviceType::XboxScarlettAnaconda;
    if (val == "lockhart" || val == "seriess") {
        dt = XSystemDeviceType::XboxScarlettLockhart;
    } else if (val == "xboxone" || val == "1080p") {
        dt = XSystemDeviceType::XboxOne;
    } else if (val == "devkit") {
        dt = XSystemDeviceType::XboxScarlettDevkit;
    } else if (val == "pc") {
        dt = XSystemDeviceType::Pc;
    }
    wsx::trace_void(kMod, "XSystemGetDeviceType", "type=%s(%u)", val.c_str(), static_cast<unsigned>(dt));
    return dt;
}

XSystemAnalyticsInfo STDAPICALLTYPE XSystemGetAnalyticsInfo() noexcept {
    XSystemAnalyticsInfo info{};
    info.osVersion = {10, 0, 26100, 1000};
    info.hostingOsVersion = {10, 0, 26100, 1000};
    const XSystemDeviceType dt = XSystemGetDeviceType();
    const std::string family = wsx::get_config_string(
        "WSX_ANALYTICS_FAMILY", "Console", "analytics_family", "Xbox");
    strncpy_s(info.family, family.c_str(), _TRUNCATE);
    switch (dt) {
    case XSystemDeviceType::XboxScarlettLockhart:
        strncpy_s(info.form, "XboxSeriesS", _TRUNCATE);
        break;
    case XSystemDeviceType::XboxScarlettDevkit:
        strncpy_s(info.form, "XboxSeriesDevkit", _TRUNCATE);
        break;
    case XSystemDeviceType::XboxOne:
        strncpy_s(info.form, "XboxOne", _TRUNCATE);
        break;
    default:
        strncpy_s(info.form, "XboxSeriesX", _TRUNCATE);
        break;
    }
    wsx::trace_void(kMod, "XSystemGetAnalyticsInfo", "family=%s form=%s", info.family, info.form);
    return info;
}

XSystemRuntimeInfo STDAPICALLTYPE XSystemGetRuntimeInfo() noexcept {
    XSystemRuntimeInfo info{};
    info.runtimeVersion = {10, 0, 26100, 2410};
    info.availableVersion = {10, 0, 26100, 2410};
    wsx::trace_void(kMod, "XSystemGetRuntimeInfo", "ver=10.0.26100.2410");
    return info;
}

HRESULT STDAPICALLTYPE XSystemGetConsoleId(
    size_t consoleIdSize,
    char* consoleId,
    size_t* consoleIdUsed) noexcept {
    // Real Xbox console ID uses 38-char 8.8.8.8.2 dot-separated hex format (39 bytes with NUL).
    const std::string cid = wsx::get_config_string(
        "WSX_CONSOLE_ID", "Console", "console_id", "00000000.00000000.00000000.00000001.01");
    const HRESULT hr = CopyStringToBuffer(cid, consoleIdSize, consoleId, consoleIdUsed);
    return wsx::trace_hr(kMod, "XSystemGetConsoleId", hr, "id=%s", cid.c_str());
}

HRESULT STDAPICALLTYPE XSystemGetAppSpecificDeviceId(
    size_t appSpecificDeviceIdSize,
    char* appSpecificDeviceId,
    size_t* appSpecificDeviceIdUsed) noexcept {
    const std::string asid = "V1NYLUFTSUQtN0I5RTJGMTEtNEQzQS00QzExLThBNUU="; // 44 chars + NUL = 45 bytes
    const HRESULT hr = CopyStringToBuffer(asid, appSpecificDeviceIdSize, appSpecificDeviceId, appSpecificDeviceIdUsed);
    return wsx::trace_hr(kMod, "XSystemGetAppSpecificDeviceId", hr, "len=%zu", asid.size());
}

HRESULT STDAPICALLTYPE XSystemGetXboxLiveSandboxId(
    size_t sandboxIdSize,
    char* sandboxId,
    size_t* sandboxIdUsed) noexcept {
    const std::string sb = wsx::get_config_string("WSX_SANDBOX_ID", "Console", "sandbox_id", "XDKS.1");
    const HRESULT hr = CopyStringToBuffer(sb, sandboxIdSize, sandboxId, sandboxIdUsed);
    return wsx::trace_hr(kMod, "XSystemGetXboxLiveSandboxId", hr, "sandbox=%s", sb.c_str());
}

bool STDAPICALLTYPE XSystemIsHandleValid(HANDLE handle) noexcept {
    return handle != nullptr && handle != INVALID_HANDLE_VALUE;
}

// ----------------------------------------------------------------------------
// XThread Exports
// ----------------------------------------------------------------------------

bool STDAPICALLTYPE XThreadIsTimeSensitive() noexcept {
    return g_thread_time_sensitive;
}

HRESULT STDAPICALLTYPE XThreadSetTimeSensitive(bool isTimeSensitive) noexcept {
    g_thread_time_sensitive = isTimeSensitive;
    return wsx::trace_hr(kMod, "XThreadSetTimeSensitive", S_OK, "sensitive=%d", isTimeSensitive ? 1 : 0);
}

void STDAPICALLTYPE XThreadAssertNotTimeSensitive() noexcept {
    // Clean-room diagnostic no-op in release; traced when WSX_TRACE=1
    if (g_thread_time_sensitive) {
        wsx::trace_void(kMod, "XThreadAssertNotTimeSensitive", "WARNING: called on time-sensitive thread");
    }
}

// ----------------------------------------------------------------------------
// PLM AppState Notifications
// ----------------------------------------------------------------------------

ULONG NTAPI WsxRegisterAppStateChangeNotification(
    PAPPSTATE_CHANGE_ROUTINE Routine,
    PVOID Context,
    PAPPSTATE_REGISTRATION* Registration) noexcept {
    if (!Registration) {
        return ERROR_INVALID_PARAMETER;
    }
    auto* node = new (std::nothrow) AppStateNode{Routine, Context};
    *Registration = reinterpret_cast<PAPPSTATE_REGISTRATION>(node);
    wsx::trace_void(kMod, "RegisterAppStateChangeNotification", "reg=%p", static_cast<void*>(node));
    return ERROR_SUCCESS;
}

VOID NTAPI WsxUnregisterAppStateChangeNotification(
    PAPPSTATE_REGISTRATION Registration) noexcept {
    delete reinterpret_cast<AppStateNode*>(Registration);
}

ULONG NTAPI WsxRegisterAppConstrainedChangeNotification(
    PAPPCONSTRAIN_CHANGE_ROUTINE Routine,
    PVOID Context,
    PAPPCONSTRAIN_REGISTRATION* Registration) noexcept {
    if (!Registration) {
        return ERROR_INVALID_PARAMETER;
    }
    auto* node = new (std::nothrow) AppConstrainNode{Routine, Context};
    *Registration = reinterpret_cast<PAPPCONSTRAIN_REGISTRATION>(node);
    return ERROR_SUCCESS;
}

VOID NTAPI WsxUnregisterAppConstrainedChangeNotification(
    PAPPCONSTRAIN_REGISTRATION Registration) noexcept {
    delete reinterpret_cast<AppConstrainNode*>(Registration);
}

// ----------------------------------------------------------------------------
// XTaskQueue Exports
// ----------------------------------------------------------------------------

HRESULT STDAPICALLTYPE XTaskQueueCreate(
    XTaskQueueDispatchMode workDispatchMode,
    XTaskQueueDispatchMode completionDispatchMode,
    XTaskQueueHandle* queue) noexcept {
    if (!queue) return wsx::trace_hr(kMod, "XTaskQueueCreate", E_INVALIDARG);
    auto* q = new (std::nothrow) XTaskQueueObject();
    if (!q) return wsx::trace_hr(kMod, "XTaskQueueCreate", E_OUTOFMEMORY);
    q->workPort.mode = workDispatchMode;
    q->completionPort.mode = completionDispatchMode;
    *queue = q;
    return wsx::trace_hr(kMod, "XTaskQueueCreate", S_OK, "work=%u comp=%u handle=%p",
                         static_cast<unsigned>(workDispatchMode),
                         static_cast<unsigned>(completionDispatchMode),
                         static_cast<void*>(q));
}

void STDAPICALLTYPE XTaskQueueCloseHandle(
    XTaskQueueHandle queue) noexcept {
    wsx::trace_void(kMod, "XTaskQueueCloseHandle", "handle=%p", static_cast<void*>(queue));
    ReleaseQueue(queue);
}

HRESULT STDAPICALLTYPE XTaskQueueDuplicateHandle(
    XTaskQueueHandle queueHandle,
    XTaskQueueHandle* duplicatedHandle) noexcept {
    if (!queueHandle || !duplicatedHandle) return E_INVALIDARG;
    RetainQueue(queueHandle);
    *duplicatedHandle = queueHandle;
    return S_OK;
}

bool STDAPICALLTYPE XTaskQueueDispatch(
    XTaskQueueHandle queue,
    XTaskQueuePort port,
    uint32_t timeoutInMs) noexcept {
    XTaskQueueHandle target = queue ? queue : GetOrCreateDefaultProcessQueue();
    XTaskQueuePort effPort = port;
    ResolveEffectivePort(target, effPort);
    return DispatchOneFromPort(target, effPort, timeoutInMs);
}

HRESULT STDAPICALLTYPE XTaskQueueSubmitCallback(
    XTaskQueueHandle queue,
    XTaskQueuePort port,
    void* callbackContext,
    XTaskQueueCallback* callback) noexcept {
    if (!callback) return E_INVALIDARG;
    XTaskQueueHandle orig = queue ? queue : GetOrCreateDefaultProcessQueue();
    if (!orig) return E_OUTOFMEMORY;
    if (orig->terminated.load(std::memory_order_acquire)) return E_ABORT;

    NotifyQueueMonitors(orig, port);

    XTaskQueueHandle target = orig;
    XTaskQueuePort effPort = port;
    ResolveEffectivePort(target, effPort);
    if (!target) return E_OUTOFMEMORY;
    if (target->terminated.load(std::memory_order_acquire)) return E_ABORT;
    if (target != orig) {
        NotifyQueueMonitors(target, effPort);
    }

    PortQueue& pq = GetPort(target, effPort);
    if (pq.mode == XTaskQueueDispatchMode::Immediate) {
        callback(callbackContext, false);
        return S_OK;
    }

    {
        std::lock_guard<std::mutex> lock(pq.mtx);
        pq.items.push_back(TaskQueueItem{callback, callbackContext});
    }
    pq.cv.notify_one();

    if (pq.mode == XTaskQueueDispatchMode::ThreadPool ||
        pq.mode == XTaskQueueDispatchMode::SerializedThreadPool) {
        auto* args = new (std::nothrow) ThreadPoolDispatchArgs{target, effPort};
        if (args) {
            RetainQueue(target);
            target->activeWorkers.fetch_add(1, std::memory_order_relaxed);
            if (!TrySubmitThreadpoolCallback(ThreadPoolPortWorker, args, nullptr)) {
                delete args;
                if (target->activeWorkers.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    std::lock_guard<std::mutex> lock(target->idleMtx);
                    target->idleCv.notify_all();
                }
                ReleaseQueue(target);
                DispatchOneFromPort(target, effPort, 0);
            }
        }
    }
    return S_OK;
}

HRESULT STDAPICALLTYPE XTaskQueueSubmitDelayedCallback(
    XTaskQueueHandle queue,
    XTaskQueuePort port,
    uint32_t delayMs,
    void* callbackContext,
    XTaskQueueCallback* callback) noexcept {
    if (!callback) return E_INVALIDARG;
    if (delayMs == 0) {
        return XTaskQueueSubmitCallback(queue, port, callbackContext, callback);
    }
    XTaskQueueHandle target = queue ? queue : GetOrCreateDefaultProcessQueue();
    if (!target) return E_OUTOFMEMORY;
    if (target->terminated.load(std::memory_order_acquire)) return E_ABORT;

    auto* node = new (std::nothrow) DelayedTimerNode();
    if (!node) return E_OUTOFMEMORY;
    node->queue = target;
    node->port = port;
    node->context = callbackContext;
    node->callback = callback;

    PTP_TIMER timer = CreateThreadpoolTimer(DelayedTimerCallback, node, nullptr);
    if (!timer) {
        delete node;
        return HRESULT_FROM_WIN32(GetLastError());
    }
    node->timer = timer;

    RetainQueue(target);
    target->activeWorkers.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(target->timerMtx);
        if (target->terminated.load(std::memory_order_acquire)) {
            CloseThreadpoolTimer(timer);
            delete node;
            if (target->activeWorkers.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                std::lock_guard<std::mutex> ilock(target->idleMtx);
                target->idleCv.notify_all();
            }
            ReleaseQueue(target);
            return E_ABORT;
        }
        target->pendingTimers.push_back(node);
    }

    FILETIME ft{};
    const int64_t due100ns = -static_cast<int64_t>(delayMs) * 10000LL;
    ft.dwLowDateTime = static_cast<DWORD>(due100ns & 0xFFFFFFFFULL);
    ft.dwHighDateTime = static_cast<DWORD>((static_cast<uint64_t>(due100ns) >> 32) & 0xFFFFFFFFULL);
    SetThreadpoolTimer(timer, &ft, 0, 0);

    return wsx::trace_hr(kMod, "XTaskQueueSubmitDelayedCallback", S_OK, "delayMs=%u port=%u",
                         delayMs, static_cast<unsigned>(port));
}

HRESULT STDAPICALLTYPE XTaskQueueTerminate(
    XTaskQueueHandle queue,
    bool wait,
    void* callbackContext,
    XTaskQueueTerminatedCallback* callback) noexcept {
    if (!queue) return E_INVALIDARG;
    queue->terminated.store(true, std::memory_order_release);
    queue->workPort.cv.notify_all();
    queue->completionPort.cv.notify_all();

    // Cancel and drain all pending ThreadPool timers cleanly
    std::vector<DelayedTimerNode*> timers_to_cancel;
    {
        std::lock_guard<std::mutex> lock(queue->timerMtx);
        timers_to_cancel.swap(queue->pendingTimers);
    }
    for (DelayedTimerNode* node : timers_to_cancel) {
        SetThreadpoolTimer(node->timer, nullptr, 0, 0);
        WaitForThreadpoolTimerCallbacks(node->timer, TRUE);
        if (!node->fired.exchange(true, std::memory_order_acq_rel)) {
            if (node->callback) {
                node->callback(node->context, true);
            }
            if (queue->activeWorkers.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                std::lock_guard<std::mutex> ilock(queue->idleMtx);
                queue->idleCv.notify_all();
            }
            ReleaseQueue(queue);
        }
        CloseThreadpoolTimer(node->timer);
        delete node;
    }

    while (DispatchOneFromPort(queue, XTaskQueuePort::Work, 0, true)) {}
    while (DispatchOneFromPort(queue, XTaskQueuePort::Completion, 0, true)) {}

    if (wait) {
        std::unique_lock<std::mutex> lock(queue->idleMtx);
        queue->idleCv.wait_for(lock, std::chrono::seconds(5), [&] {
            return queue->activeWorkers.load(std::memory_order_acquire) <= 0;
        });
    }

    if (callback) {
        callback(callbackContext);
    }
    return wsx::trace_hr(kMod, "XTaskQueueTerminate", S_OK, "handle=%p wait=%d",
                         static_cast<void*>(queue), wait ? 1 : 0);
}

bool STDAPICALLTYPE XTaskQueueGetCurrentProcessTaskQueue(
    XTaskQueueHandle* queue) noexcept {
    if (!queue) return false;
    XTaskQueueHandle q = GetOrCreateDefaultProcessQueue();
    if (!q) {
        *queue = nullptr;
        return false;
    }
    RetainQueue(q);
    *queue = q;
    return true;
}

void STDAPICALLTYPE XTaskQueueSetCurrentProcessTaskQueue(
    XTaskQueueHandle queue) noexcept {
    std::lock_guard<std::mutex> lock(g_process_queue_mtx);
    if (g_process_queue) {
        ReleaseQueue(g_process_queue);
    }
    g_process_queue = queue;
    if (g_process_queue) {
        RetainQueue(g_process_queue);
    }
}

HRESULT STDAPICALLTYPE XTaskQueueGetPort(
    XTaskQueueHandle queue,
    XTaskQueuePort port,
    XTaskQueuePortHandle* portHandle) noexcept {
    if (!queue || !portHandle) return E_INVALIDARG;
    *portHandle = (port == XTaskQueuePort::Work)
        ? &queue->workPortHandleObj
        : &queue->completionPortHandleObj;
    return S_OK;
}

HRESULT STDAPICALLTYPE XTaskQueueCreateComposite(
    XTaskQueuePortHandle workPort,
    XTaskQueuePortHandle completionPort,
    XTaskQueueHandle* queue) noexcept {
    if (!workPort || !completionPort || !queue || !workPort->owner || !completionPort->owner) {
        return E_INVALIDARG;
    }
    auto* q = new (std::nothrow) XTaskQueueObject();
    if (!q) return E_OUTOFMEMORY;
    q->workDelegateQueue = workPort->owner;
    q->workDelegatePort = workPort->port;
    RetainQueue(q->workDelegateQueue);
    q->completionDelegateQueue = completionPort->owner;
    q->completionDelegatePort = completionPort->port;
    RetainQueue(q->completionDelegateQueue);
    q->workPort.mode = GetPort(workPort->owner, workPort->port).mode;
    q->completionPort.mode = GetPort(completionPort->owner, completionPort->port).mode;
    *queue = q;
    return wsx::trace_hr(kMod, "XTaskQueueCreateComposite", S_OK, "handle=%p", static_cast<void*>(q));
}

HRESULT STDAPICALLTYPE XTaskQueueRegisterMonitor(
    XTaskQueueHandle queue,
    void* callbackContext,
    XTaskQueueMonitorCallback* callback,
    XTaskQueueRegistrationToken* token) noexcept {
    if (!queue || !callback || !token) return E_INVALIDARG;
    const uint64_t tok = g_next_token.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(queue->monitorMtx);
    queue->monitors.push_back(TaskQueueMonitorReg{tok, callbackContext, callback});
    token->token = tok;
    return S_OK;
}

void STDAPICALLTYPE XTaskQueueUnregisterMonitor(
    XTaskQueueHandle queue,
    XTaskQueueRegistrationToken token) noexcept {
    if (!queue) return;
    std::lock_guard<std::mutex> lk(queue->monitorMtx);
    for (auto it = queue->monitors.begin(); it != queue->monitors.end(); ++it) {
        if (it->token == token.token) {
            queue->monitors.erase(it);
            return;
        }
    }
}

// ----------------------------------------------------------------------------
// XAsync & XAsyncProvider Exports
// ----------------------------------------------------------------------------

HRESULT XAsyncBeginInternal(
    XAsyncBlock* asyncBlock,
    void* context,
    bool ownsContext,
    const void* identity,
    const char* identityName,
    XAsyncProvider* provider) noexcept {
    if (!asyncBlock || !provider) {
        if (ownsContext && context) std::free(context);
        return E_INVALIDARG;
    }

    auto* state = new (std::nothrow) WsxAsyncState();
    if (!state) {
        if (ownsContext && context) std::free(context);
        return E_OUTOFMEMORY;
    }

    state->asyncBlock = asyncBlock;
    state->queue = asyncBlock->queue ? asyncBlock->queue : GetOrCreateDefaultProcessQueue();
    RetainQueue(state->queue);
    state->providerContext = context;
    state->ownsProviderContext = ownsContext;
    state->identity = identity;
    state->identityName = identityName;
    state->provider = provider;

    auto* layout = reinterpret_cast<AsyncInternalLayout*>(asyncBlock->internal);
    layout->magic = kWsxAsyncMagic;
    layout->state = state;

    XAsyncProviderData data{};
    data.async = asyncBlock;
    data.context = context;
    const HRESULT hr = provider(XAsyncOp::Begin, &data);
    if (FAILED(hr) && hr != E_NOTIMPL && hr != E_PENDING) {
        layout->magic = 0;
        layout->state = nullptr;
        ReleaseQueue(state->queue);
        if (ownsContext && context) std::free(context);
        delete state;
        return hr;
    }
    return S_OK;
}

HRESULT STDAPICALLTYPE XAsyncBegin(
    XAsyncBlock* asyncBlock,
    void* context,
    const void* identity,
    const char* identityName,
    XAsyncProvider* provider) noexcept {
    return XAsyncBeginInternal(asyncBlock, context, false, identity, identityName, provider);
}

HRESULT STDAPICALLTYPE XAsyncSchedule(
    XAsyncBlock* asyncBlock,
    uint32_t delayInMs) noexcept {
    WsxAsyncState* state = GetAsyncState(asyncBlock);
    if (!state) return E_INVALIDARG;
    return XTaskQueueSubmitDelayedCallback(
        state->queue, XTaskQueuePort::Work, delayInMs, state, AsyncWorkCallback);
}

void STDAPICALLTYPE XAsyncComplete(
    XAsyncBlock* asyncBlock,
    HRESULT result,
    size_t requiredBufferSize) noexcept {
    WsxAsyncState* state = GetAsyncState(asyncBlock);
    if (!state) return;
    if (state->completing.exchange(true, std::memory_order_acq_rel)) return;

    state->requiredBufferSize.store(requiredBufferSize, std::memory_order_release);
    state->status.store(result, std::memory_order_release);

    // Queue the completion callback before waking XAsyncGetStatus waiters so
    // ThreadPool completion workers do not race past caller stack teardown.
    const HRESULT submitHr = XTaskQueueSubmitCallback(
        state->queue, XTaskQueuePort::Completion, state, AsyncCompletionCallback);

    state->workDone.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(state->mtx);
        state->cv.notify_all();
    }

    if (FAILED(submitHr)) {
        FireAsyncCompletionIfNeeded(state);
    }
}

HRESULT STDAPICALLTYPE XAsyncGetStatus(
    XAsyncBlock* asyncBlock,
    bool wait) noexcept {
    WsxAsyncState* state = GetAsyncState(asyncBlock);
    if (!state) return E_INVALIDARG;

    if (wait) {
        const bool manualWork =
            state->queue && state->queue->workPort.mode == XTaskQueueDispatchMode::Manual;
        while (!state->workDone.load(std::memory_order_acquire)) {
            if (manualWork) {
                if (!DispatchOneFromPort(state->queue, XTaskQueuePort::Work, 1)) {
                    std::unique_lock<std::mutex> lock(state->mtx);
                    if (!state->workDone.load(std::memory_order_acquire)) {
                        state->cv.wait_for(lock, std::chrono::milliseconds(2));
                    }
                }
            } else {
                std::unique_lock<std::mutex> lock(state->mtx);
                if (!state->workDone.load(std::memory_order_acquire)) {
                    state->cv.wait_for(lock, std::chrono::milliseconds(2));
                }
            }
        }

        const bool manualComp =
            state->queue && state->queue->completionPort.mode == XTaskQueueDispatchMode::Manual;
        while (!state->completionDone.load(std::memory_order_acquire)) {
            if (state->completionFired.load(std::memory_order_acquire)) {
                if (state->completionThreadId.load(std::memory_order_acquire) == GetCurrentThreadId()) {
                    break;
                }
                std::unique_lock<std::mutex> lock(state->mtx);
                if (!state->completionDone.load(std::memory_order_acquire)) {
                    state->cv.wait_for(lock, std::chrono::milliseconds(2));
                }
            } else if (manualComp) {
                if (!DispatchOneFromPort(state->queue, XTaskQueuePort::Completion, 0)) {
                    FireAsyncCompletionIfNeeded(state);
                    break;
                }
            } else if (state->queue && state->queue->terminated.load(std::memory_order_acquire)) {
                FireAsyncCompletionIfNeeded(state);
                break;
            } else {
                std::unique_lock<std::mutex> lock(state->mtx);
                if (!state->completionDone.load(std::memory_order_acquire)) {
                    state->cv.wait_for(lock, std::chrono::milliseconds(2));
                }
            }
        }
    }
    return state->status.load(std::memory_order_acquire);
}

HRESULT STDAPICALLTYPE XAsyncGetResultSize(
    XAsyncBlock* asyncBlock,
    size_t* bufferSize) noexcept {
    if (!bufferSize) return E_INVALIDARG;
    *bufferSize = 0;
    const HRESULT hr = XAsyncGetStatus(asyncBlock, false);
    if (FAILED(hr)) return hr;
    WsxAsyncState* state = GetAsyncState(asyncBlock);
    if (!state) return E_INVALIDARG;
    *bufferSize = state->requiredBufferSize.load(std::memory_order_acquire);
    return S_OK;
}

HRESULT STDAPICALLTYPE XAsyncGetResult(
    XAsyncBlock* asyncBlock,
    const void* identity,
    size_t bufferSize,
    void* buffer,
    size_t* bufferUsed) noexcept {
    WsxAsyncState* state = GetAsyncState(asyncBlock);
    if (!state) return E_INVALIDARG;
    if (identity && state->identity && identity != state->identity) {
        return E_INVALIDARG;
    }

    const HRESULT status = XAsyncGetStatus(asyncBlock, true);
    if (FAILED(status)) return status;

    const size_t reqSize = state->requiredBufferSize.load(std::memory_order_acquire);
    if (bufferUsed) *bufferUsed = reqSize;
    if (reqSize > 0 && (!buffer || bufferSize < reqSize)) {
        return E_NOT_SUFFICIENT_BUFFER;
    }

    HRESULT hr = S_OK;
    if (reqSize > 0 && state->provider && !state->cleanedUp.load(std::memory_order_acquire)) {
        XAsyncProviderData data{};
        data.async = asyncBlock;
        data.bufferSize = bufferSize;
        data.buffer = buffer;
        data.context = state->providerContext;
        hr = state->provider(XAsyncOp::GetResult, &data);
    }
    CleanupAsyncProviderOnce(state);
    state->resultRetrieved.store(true, std::memory_order_release);
    return hr;
}

void STDAPICALLTYPE XAsyncCancel(
    XAsyncBlock* asyncBlock) noexcept {
    WsxAsyncState* state = GetAsyncState(asyncBlock);
    if (!state) return;
    state->canceled.store(true, std::memory_order_release);
    if (state->provider) {
        XAsyncProviderData data{};
        data.async = asyncBlock;
        data.context = state->providerContext;
        state->provider(XAsyncOp::Cancel, &data);
    }
}

HRESULT STDAPICALLTYPE XAsyncRun(
    XAsyncBlock* asyncBlock,
    XAsyncWork* work) noexcept {
    if (!asyncBlock || !work) return E_INVALIDARG;
    const HRESULT hr = XAsyncBegin(
        asyncBlock, reinterpret_cast<void*>(work), nullptr, "XAsyncRun",
        [](XAsyncOp op, const XAsyncProviderData* data) noexcept -> HRESULT {
            if (op == XAsyncOp::DoWork) {
                auto* fn = reinterpret_cast<XAsyncWork*>(data->context);
                const HRESULT workHr = fn(data->async);
                XAsyncComplete(data->async, workHr, 0);
                return S_OK;
            }
            return S_OK;
        });
    if (FAILED(hr)) return hr;
    return XAsyncSchedule(asyncBlock, 0);
}

// ----------------------------------------------------------------------------
// XUser Exports
// ----------------------------------------------------------------------------

HRESULT STDAPICALLTYPE XUserAddAsync(
    XUserAddOptions options,
    XAsyncBlock* async) noexcept {
    if (!async) return wsx::trace_hr(kMod, "XUserAddAsync", E_INVALIDARG);
    const HRESULT hr = XAsyncBegin(
        async, nullptr, reinterpret_cast<const void*>(&XUserAddAsync), "XUserAddAsync",
        [](XAsyncOp op, const XAsyncProviderData* data) noexcept -> HRESULT {
            if (op == XAsyncOp::DoWork) {
                XAsyncComplete(data->async, S_OK, sizeof(XUserHandle));
                return S_OK;
            }
            if (op == XAsyncOp::GetResult) {
                if (!data->buffer || data->bufferSize < sizeof(XUserHandle)) {
                    return E_INVALIDARG;
                }
                XUserHandle u = GetOrCreateDefaultUser();
                if (!u) return E_OUTOFMEMORY;
                *static_cast<XUserHandle*>(data->buffer) = u;
                return S_OK;
            }
            return S_OK;
        });
    if (FAILED(hr)) return wsx::trace_hr(kMod, "XUserAddAsync", hr);
    const HRESULT schedHr = XAsyncSchedule(async, 0);
    return wsx::trace_hr(kMod, "XUserAddAsync", schedHr, "options=0x%X", static_cast<unsigned>(options));
}

HRESULT STDAPICALLTYPE XUserAddResult(
    XAsyncBlock* async,
    XUserHandle* newUser) noexcept {
    if (!async || !newUser) return E_INVALIDARG;
    *newUser = nullptr;
    const HRESULT hr = XAsyncGetResult(
        async, reinterpret_cast<const void*>(&XUserAddAsync),
        sizeof(XUserHandle), newUser, nullptr);
    return wsx::trace_hr(kMod, "XUserAddResult", hr, "user=%p", static_cast<void*>(*newUser));
}

HRESULT STDAPICALLTYPE XUserDuplicateHandle(
    XUserHandle handle,
    XUserHandle* duplicatedHandle) noexcept {
    if (!IsValidUser(handle) || !duplicatedHandle) return E_INVALIDARG;
    handle->refCount.fetch_add(1, std::memory_order_relaxed);
    *duplicatedHandle = handle;
    return S_OK;
}

void STDAPICALLTYPE XUserCloseHandle(
    XUserHandle user) noexcept {
    if (!IsValidUser(user)) return;
    if (user->refCount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        std::lock_guard<std::mutex> lock(g_user_mtx);
        if (g_default_user == user) g_default_user = nullptr;
        delete user;
    }
}

int32_t STDAPICALLTYPE XUserCompare(
    XUserHandle user1,
    XUserHandle user2) noexcept {
    if (user1 == user2) return 0;
    if (!user1) return -1;
    if (!user2) return 1;
    if (user1->xuid < user2->xuid) return -1;
    return (user1->xuid > user2->xuid) ? 1 : 0;
}

HRESULT STDAPICALLTYPE XUserGetMaxUsers(
    uint32_t* maxUsers) noexcept {
    if (!maxUsers) return E_INVALIDARG;
    *maxUsers = 8;
    return S_OK;
}

HRESULT STDAPICALLTYPE XUserGetId(
    XUserHandle user,
    uint64_t* userId) noexcept {
    if (!IsValidUser(user) || !userId) return E_INVALIDARG;
    *userId = user->xuid;
    return wsx::trace_hr(kMod, "XUserGetId", S_OK, "xuid=0x%016llX", static_cast<unsigned long long>(*userId));
}

HRESULT STDAPICALLTYPE XUserGetLocalId(
    XUserHandle user,
    XUserLocalId* userLocalId) noexcept {
    if (!IsValidUser(user) || !userLocalId) return E_INVALIDARG;
    *userLocalId = user->localId;
    return S_OK;
}

HRESULT STDAPICALLTYPE XUserFindUserById(
    uint64_t userId,
    XUserHandle* handle) noexcept {
    if (!handle) return E_INVALIDARG;
    *handle = nullptr;
    XUserHandle def = GetOrCreateDefaultUser();
    if (def && def->xuid == userId) {
        *handle = def;
        return S_OK;
    }
    if (def) XUserCloseHandle(def);
    return E_FAIL;
}

HRESULT STDAPICALLTYPE XUserFindUserByLocalId(
    XUserLocalId userLocalId,
    XUserHandle* handle) noexcept {
    if (!handle) return E_INVALIDARG;
    *handle = nullptr;
    XUserHandle def = GetOrCreateDefaultUser();
    if (def && def->localId.value == userLocalId.value) {
        *handle = def;
        return S_OK;
    }
    if (def) XUserCloseHandle(def);
    return E_FAIL;
}

HRESULT STDAPICALLTYPE XUserGetState(
    XUserHandle user,
    XUserState* state) noexcept {
    if (!IsValidUser(user) || !state) return E_INVALIDARG;
    *state = user->state;
    return S_OK;
}

HRESULT STDAPICALLTYPE XUserGetGamertag(
    XUserHandle user,
    XUserGamertagComponent gamertagComponent,
    size_t gamertagSize,
    char* gamertag,
    size_t* gamertagUsed) noexcept {
    if (!IsValidUser(user)) return E_INVALIDARG;
    const std::string* src = &user->modernGamertag;
    switch (gamertagComponent) {
        case XUserGamertagComponent::Classic:
        case XUserGamertagComponent::Modern:
            src = &user->classicGamertag;
            break;
        case XUserGamertagComponent::ModernSuffix:
            src = &user->suffix;
            break;
        case XUserGamertagComponent::UniqueModern:
        default:
            src = &user->modernGamertag;
            break;
    }
    const HRESULT hr = CopyStringToBuffer(*src, gamertagSize, gamertag, gamertagUsed);
    return wsx::trace_hr(kMod, "XUserGetGamertag", hr, "comp=%u tag=%s",
                         static_cast<unsigned>(gamertagComponent), src->c_str());
}

HRESULT STDAPICALLTYPE XUserGetAgeGroup(
    XUserHandle user,
    XUserAgeGroup* ageGroup) noexcept {
    if (!IsValidUser(user) || !ageGroup) return E_INVALIDARG;
    *ageGroup = XUserAgeGroup::Adult;
    return S_OK;
}

HRESULT STDAPICALLTYPE XUserCheckPrivilege(
    XUserHandle user,
    XUserPrivilegeOptions /*options*/,
    XUserPrivilege privilege,
    bool* hasPrivilege,
    XUserPrivilegeDenyReason* reason) noexcept {
    if (!IsValidUser(user) || !hasPrivilege) return E_INVALIDARG;
    *hasPrivilege = true;
    if (reason) *reason = XUserPrivilegeDenyReason::None;
    return wsx::trace_hr(kMod, "XUserCheckPrivilege", S_OK, "priv=%u allowed=1", static_cast<unsigned>(privilege));
}

HRESULT STDAPICALLTYPE XUserIsStoreUser(
    XUserHandle user,
    bool* isStoreUser) noexcept {
    if (!IsValidUser(user) || !isStoreUser) return E_INVALIDARG;
    *isStoreUser = true;
    return S_OK;
}

HRESULT STDAPICALLTYPE XUserRegisterForChangeEvent(
    XTaskQueueHandle queue,
    void* context,
    XUserChangeEventCallback* callback,
    XTaskQueueRegistrationToken* token) noexcept {
    if (!callback || !token) return E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_user_mtx);
    const uint64_t t = g_next_token.fetch_add(1, std::memory_order_relaxed);
    g_user_change_regs.push_back(UserChangeReg{t, queue, context, callback});
    token->token = t;
    return wsx::trace_hr(kMod, "XUserRegisterForChangeEvent", S_OK, "token=%llu", static_cast<unsigned long long>(t));
}

bool STDAPICALLTYPE XUserUnregisterForChangeEvent(
    XTaskQueueRegistrationToken token,
    bool /*wait*/) noexcept {
    if (token.token == 0) return false;
    std::lock_guard<std::mutex> lock(g_user_mtx);
    auto it = std::remove_if(g_user_change_regs.begin(), g_user_change_regs.end(),
                             [&](const UserChangeReg& r) { return r.token == token.token; });
    if (it == g_user_change_regs.end()) return false;
    g_user_change_regs.erase(it, g_user_change_regs.end());
    return true;
}

// ----------------------------------------------------------------------------
// XGameSave & XGameSaveFiles Exports (Atomic Writes + Real Quota)
// ----------------------------------------------------------------------------

HRESULT STDAPICALLTYPE XGameSaveInitializeProvider(
    XUserHandle requestingUser,
    const char* configurationId,
    bool /*syncOnDemand*/,
    XGameSaveProviderHandle* provider) noexcept {
    if (!provider) return E_INVALIDARG;
    *provider = nullptr;

    uint64_t xuid = wsx::get_config_uint64("WSX_XUID", "User", "xuid", 0x0009000000000001ULL);
    if (IsValidUser(requestingUser)) {
        xuid = requestingUser->xuid;
    }

    const std::string scid = SanitizePathComponent(configurationId ? configurationId : "00000000-0000-0000-0000-000000000000");
    char xuidHex[32]{};
    std::snprintf(xuidHex, sizeof(xuidHex), "%016llX", static_cast<unsigned long long>(xuid));

    auto* p = new (std::nothrow) XGameSaveProvider();
    if (!p) return E_OUTOFMEMORY;
    p->xuid = xuid;
    p->scid = scid;
    p->rootDir = GetBaseSaveDirectory() / scid / xuidHex;
    const int64_t quotaMb = std::max<int64_t>(16, wsx::get_config_int("WSX_GAMESAVE_QUOTA_MB", "Storage", "gamesave_quota_mb", 256));
    p->maxQuotaBytes = quotaMb * 1024LL * 1024LL;

    std::error_code ec;
    fs::create_directories(p->rootDir, ec);
    if (ec) {
        delete p;
        return wsx::trace_hr(kMod, "XGameSaveInitializeProvider", E_GS_OUT_OF_LOCAL_STORAGE, "scid=%s", scid.c_str());
    }

    *provider = p;
    return wsx::trace_hr(kMod, "XGameSaveInitializeProvider", S_OK, "scid=%s xuid=%s", scid.c_str(), xuidHex);
}

struct InitProviderAsyncArgs {
    XUserHandle user;
    std::string scid;
    bool syncOnDemand;
    XGameSaveProviderHandle provider = nullptr;
};

HRESULT STDAPICALLTYPE XGameSaveInitializeProviderAsync(
    XUserHandle requestingUser,
    const char* configurationId,
    bool syncOnDemand,
    XAsyncBlock* async) noexcept {
    if (!async) return E_INVALIDARG;
    auto* args = new (std::nothrow) InitProviderAsyncArgs{
        requestingUser, configurationId ? configurationId : "", syncOnDemand, nullptr};
    if (!args) return E_OUTOFMEMORY;

    const HRESULT hr = XAsyncBegin(
        async, args, reinterpret_cast<const void*>(&XGameSaveInitializeProviderAsync),
        "XGameSaveInitializeProviderAsync",
        [](XAsyncOp op, const XAsyncProviderData* data) noexcept -> HRESULT {
            auto* a = static_cast<InitProviderAsyncArgs*>(data->context);
            if (op == XAsyncOp::DoWork) {
                const HRESULT initHr = XGameSaveInitializeProvider(
                    a->user, a->scid.c_str(), a->syncOnDemand, &a->provider);
                XAsyncComplete(data->async, initHr, sizeof(XGameSaveProviderHandle));
                return S_OK;
            }
            if (op == XAsyncOp::GetResult) {
                if (!data->buffer || data->bufferSize < sizeof(XGameSaveProviderHandle)) {
                    return E_INVALIDARG;
                }
                *static_cast<XGameSaveProviderHandle*>(data->buffer) = a->provider;
                a->provider = nullptr;
                return S_OK;
            }
            if (op == XAsyncOp::Cleanup) {
                if (a->provider) XGameSaveCloseProvider(a->provider);
                delete a;
            }
            return S_OK;
        });
    if (FAILED(hr)) {
        delete args;
        return hr;
    }
    return wsx::trace_hr(kMod, "XGameSaveInitializeProviderAsync", XAsyncSchedule(async, 0),
                         "scid=%s", configurationId ? configurationId : "");
}

HRESULT STDAPICALLTYPE XGameSaveInitializeProviderResult(
    XAsyncBlock* async,
    XGameSaveProviderHandle* provider) noexcept {
    if (!async || !provider) return E_INVALIDARG;
    *provider = nullptr;
    return XAsyncGetResult(
        async, reinterpret_cast<const void*>(&XGameSaveInitializeProviderAsync),
        sizeof(XGameSaveProviderHandle), provider, nullptr);
}

void STDAPICALLTYPE XGameSaveCloseProvider(
    XGameSaveProviderHandle provider) noexcept {
    delete provider;
}

HRESULT STDAPICALLTYPE XGameSaveGetRemainingQuota(
    XGameSaveProviderHandle provider,
    int64_t* remainingQuota) noexcept {
    if (!provider || !remainingQuota) return E_INVALIDARG;
    const uint64_t used = ComputeDirectoryUsedBytes(provider->rootDir, kMetaFileName);
    const int64_t quotaLeft = (static_cast<int64_t>(used) < provider->maxQuotaBytes)
        ? (provider->maxQuotaBytes - static_cast<int64_t>(used))
        : 0LL;
    const uint64_t diskFree = QueryDiskFreeBytes(provider->rootDir);
    *remainingQuota = static_cast<int64_t>(std::min<uint64_t>(static_cast<uint64_t>(quotaLeft), diskFree));
    return wsx::trace_hr(kMod, "XGameSaveGetRemainingQuota", S_OK, "used=%llu rem=%lld",
                         static_cast<unsigned long long>(used),
                         static_cast<long long>(*remainingQuota));
}

HRESULT STDAPICALLTYPE XGameSaveGetRemainingQuotaAsync(
    XGameSaveProviderHandle provider,
    XAsyncBlock* async) noexcept {
    if (!provider || !async) return E_INVALIDARG;
    const HRESULT hr = XAsyncBegin(
        async, provider, reinterpret_cast<const void*>(&XGameSaveGetRemainingQuotaAsync),
        "XGameSaveGetRemainingQuotaAsync",
        [](XAsyncOp op, const XAsyncProviderData* data) noexcept -> HRESULT {
            if (op == XAsyncOp::DoWork) {
                XAsyncComplete(data->async, S_OK, sizeof(int64_t));
                return S_OK;
            }
            if (op == XAsyncOp::GetResult) {
                int64_t quota = 0;
                XGameSaveGetRemainingQuota(static_cast<XGameSaveProviderHandle>(data->context), &quota);
                *static_cast<int64_t*>(data->buffer) = quota;
                return S_OK;
            }
            return S_OK;
        });
    if (FAILED(hr)) return hr;
    return XAsyncSchedule(async, 0);
}

HRESULT STDAPICALLTYPE XGameSaveGetRemainingQuotaResult(
    XAsyncBlock* async,
    int64_t* remainingQuota) noexcept {
    if (!async || !remainingQuota) return E_INVALIDARG;
    return XAsyncGetResult(
        async, reinterpret_cast<const void*>(&XGameSaveGetRemainingQuotaAsync),
        sizeof(int64_t), remainingQuota, nullptr);
}

HRESULT STDAPICALLTYPE XGameSaveCreateContainer(
    XGameSaveProviderHandle provider,
    const char* containerName,
    XGameSaveContainerHandle* containerContext) noexcept {
    if (!provider || !containerName || !containerContext) return E_INVALIDARG;
    if (containerName[0] == '\0' || std::strlen(containerName) > X_GS_MAX_CONTAINER_NAME_SIZE) {
        return E_GS_INVALID_CONTAINER_NAME;
    }
    auto* c = new (std::nothrow) XGameSaveContainer();
    if (!c) return E_OUTOFMEMORY;
    c->provider = provider;
    c->name = containerName;
    c->containerDir = provider->rootDir / SanitizePathComponent(containerName);
    *containerContext = c;
    return wsx::trace_hr(kMod, "XGameSaveCreateContainer", S_OK, "name=%s", containerName);
}

void STDAPICALLTYPE XGameSaveCloseContainer(
    XGameSaveContainerHandle containerContext) noexcept {
    delete containerContext;
}

HRESULT STDAPICALLTYPE XGameSaveDeleteContainer(
    XGameSaveProviderHandle provider,
    const char* containerName) noexcept {
    if (!provider || !containerName || containerName[0] == '\0') return E_INVALIDARG;
    std::error_code ec;
    fs::remove_all(provider->rootDir / SanitizePathComponent(containerName), ec);
    return wsx::trace_hr(kMod, "XGameSaveDeleteContainer", S_OK, "name=%s", containerName);
}

struct DeleteContainerAsyncArgs {
    XGameSaveProviderHandle provider;
    std::string containerName;
};

HRESULT STDAPICALLTYPE XGameSaveDeleteContainerAsync(
    XGameSaveProviderHandle provider,
    const char* containerName,
    XAsyncBlock* async) noexcept {
    if (!provider || !containerName || !async) return E_INVALIDARG;
    auto* args = new (std::nothrow) DeleteContainerAsyncArgs{provider, containerName};
    if (!args) return E_OUTOFMEMORY;

    const HRESULT hr = XAsyncBegin(
        async, args, reinterpret_cast<const void*>(&XGameSaveDeleteContainerAsync),
        "XGameSaveDeleteContainerAsync",
        [](XAsyncOp op, const XAsyncProviderData* data) noexcept -> HRESULT {
            auto* a = static_cast<DeleteContainerAsyncArgs*>(data->context);
            if (op == XAsyncOp::DoWork) {
                const HRESULT delHr = XGameSaveDeleteContainer(a->provider, a->containerName.c_str());
                XAsyncComplete(data->async, delHr, 0);
                return S_OK;
            }
            if (op == XAsyncOp::Cleanup) {
                delete a;
            }
            return S_OK;
        });
    if (FAILED(hr)) {
        delete args;
        return hr;
    }
    return XAsyncSchedule(async, 0);
}

HRESULT STDAPICALLTYPE XGameSaveDeleteContainerResult(
    XAsyncBlock* async) noexcept {
    return XAsyncGetResult(
        async, reinterpret_cast<const void*>(&XGameSaveDeleteContainerAsync),
        0, nullptr, nullptr);
}

HRESULT STDAPICALLTYPE XGameSaveCreateUpdate(
    XGameSaveContainerHandle container,
    const char* containerDisplayName,
    XGameSaveUpdateHandle* updateContext) noexcept {
    if (!container || !updateContext) return E_INVALIDARG;
    auto* u = new (std::nothrow) XGameSaveUpdate();
    if (!u) return E_OUTOFMEMORY;
    u->container = container;
    u->displayName = containerDisplayName ? containerDisplayName : container->name;
    *updateContext = u;
    return S_OK;
}

void STDAPICALLTYPE XGameSaveCloseUpdate(
    XGameSaveUpdateHandle updateContext) noexcept {
    delete updateContext;
}

HRESULT STDAPICALLTYPE XGameSaveSubmitBlobWrite(
    XGameSaveUpdateHandle updateContext,
    const char* blobName,
    const uint8_t* data,
    size_t dataSize) noexcept {
    if (!updateContext || !blobName || blobName[0] == '\0' || (!data && dataSize > 0)) {
        return E_INVALIDARG;
    }
    PendingBlobOp op{};
    op.isDelete = false;
    op.blobName = blobName;
    if (data && dataSize > 0) {
        op.data.assign(data, data + dataSize);
    }
    updateContext->ops.push_back(std::move(op));
    return wsx::trace_hr(kMod, "XGameSaveSubmitBlobWrite", S_OK, "blob=%s bytes=%zu", blobName, dataSize);
}

HRESULT STDAPICALLTYPE XGameSaveSubmitBlobDelete(
    XGameSaveUpdateHandle updateContext,
    const char* blobName) noexcept {
    if (!updateContext || !blobName || blobName[0] == '\0') return E_INVALIDARG;
    PendingBlobOp op{};
    op.isDelete = true;
    op.blobName = blobName;
    updateContext->ops.push_back(std::move(op));
    return S_OK;
}

HRESULT STDAPICALLTYPE XGameSaveSubmitUpdate(
    XGameSaveUpdateHandle updateContext) noexcept {
    if (!updateContext || !updateContext->container) return E_INVALIDARG;
    constexpr size_t kMaxSingleUpdateBytes = 16u * 1024u * 1024u; // 16 MiB GDK Connected Storage limit

    // 1. Pre-validate total batch size and projected quota BEFORE touching any blob on disk
    size_t batchWriteBytes = 0;
    int64_t projectedDelta = 0;
    const fs::path& dir = updateContext->container->containerDir;
    std::error_code ec;
    for (const auto& op : updateContext->ops) {
        const fs::path blobPath = dir / SanitizePathComponent(op.blobName);
        const int64_t existingSz = fs::is_regular_file(blobPath, ec)
            ? static_cast<int64_t>(fs::file_size(blobPath, ec))
            : 0;
        if (op.isDelete) {
            projectedDelta -= existingSz;
        } else {
            batchWriteBytes += op.data.size();
            if (batchWriteBytes > kMaxSingleUpdateBytes) {
                return wsx::trace_hr(kMod, "XGameSaveSubmitUpdate", E_GS_UPDATE_TOO_BIG,
                                     "container=%s bytes=%zu",
                                     updateContext->container->name.c_str(), batchWriteBytes);
            }
            projectedDelta += static_cast<int64_t>(op.data.size()) - existingSz;
        }
    }

    if (updateContext->container->provider && projectedDelta > 0) {
        const uint64_t used = ComputeDirectoryUsedBytes(updateContext->container->provider->rootDir, kMetaFileName);
        if (static_cast<int64_t>(used) + projectedDelta > updateContext->container->provider->maxQuotaBytes) {
            return wsx::trace_hr(kMod, "XGameSaveSubmitUpdate", E_GS_QUOTA_EXCEEDED,
                                 "container=%s delta=%lld",
                                 updateContext->container->name.c_str(),
                                 static_cast<long long>(projectedDelta));
        }
    }

    fs::create_directories(dir, ec);
    if (ec) return E_GS_OUT_OF_LOCAL_STORAGE;

    // 2. Stage ALL blob writes to temporary files first (all-or-nothing transactional commit)
    struct StagedCommit {
        fs::path tmpPath;
        fs::path finalPath;
    };
    std::vector<StagedCommit> staged;
    std::vector<fs::path> deletes;
    static std::atomic<uint64_t> txSeq{1};
    const uint64_t txId = txSeq.fetch_add(1, std::memory_order_relaxed);
    const DWORD tid = GetCurrentThreadId();

    size_t idx = 0;
    for (const auto& op : updateContext->ops) {
        const fs::path finalPath = dir / SanitizePathComponent(op.blobName);
        if (op.isDelete) {
            deletes.push_back(finalPath);
            continue;
        }
        wchar_t suffix[64]{};
        std::swprintf(suffix, std::size(suffix), L".wsx_tx.%lu.%llu.%zu",
                      static_cast<unsigned long>(tid),
                      static_cast<unsigned long long>(txId), idx++);
        fs::path tmpPath = finalPath;
        tmpPath += suffix;
        {
            std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
            if (!out) {
                for (const auto& s : staged) DeleteFileW(s.tmpPath.c_str());
                return E_GS_OUT_OF_LOCAL_STORAGE;
            }
            if (!op.data.empty()) {
                out.write(reinterpret_cast<const char*>(op.data.data()),
                          static_cast<std::streamsize>(op.data.size()));
            }
            out.flush();
            if (!out.good()) {
                out.close();
                DeleteFileW(tmpPath.c_str());
                for (const auto& s : staged) DeleteFileW(s.tmpPath.c_str());
                return E_GS_OUT_OF_LOCAL_STORAGE;
            }
        }
        staged.push_back(StagedCommit{std::move(tmpPath), finalPath});
    }

    // 3. Commit staged renames and deletes
    for (const auto& s : staged) {
        if (!MoveFileExW(s.tmpPath.c_str(), s.finalPath.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            for (const auto& rem : staged) DeleteFileW(rem.tmpPath.c_str());
            return E_GS_OUT_OF_LOCAL_STORAGE;
        }
    }
    for (const auto& dp : deletes) {
        fs::remove(dp, ec);
    }

    WriteContainerDisplayName(dir, updateContext->displayName);
    return wsx::trace_hr(kMod, "XGameSaveSubmitUpdate", S_OK, "container=%s ops=%zu tx=1",
                         updateContext->container->name.c_str(), updateContext->ops.size());
}

HRESULT STDAPICALLTYPE XGameSaveSubmitUpdateAsync(
    XGameSaveUpdateHandle updateContext,
    XAsyncBlock* async) noexcept {
    if (!updateContext || !async) return E_INVALIDARG;
    const HRESULT hr = XAsyncBegin(
        async, updateContext, reinterpret_cast<const void*>(&XGameSaveSubmitUpdateAsync),
        "XGameSaveSubmitUpdateAsync",
        [](XAsyncOp op, const XAsyncProviderData* data) noexcept -> HRESULT {
            if (op == XAsyncOp::DoWork) {
                const HRESULT subHr = XGameSaveSubmitUpdate(
                    static_cast<XGameSaveUpdateHandle>(data->context));
                XAsyncComplete(data->async, subHr, 0);
                return S_OK;
            }
            return S_OK;
        });
    if (FAILED(hr)) return hr;
    return XAsyncSchedule(async, 0);
}

HRESULT STDAPICALLTYPE XGameSaveSubmitUpdateResult(
    XAsyncBlock* async) noexcept {
    return XAsyncGetResult(
        async, reinterpret_cast<const void*>(&XGameSaveSubmitUpdateAsync),
        0, nullptr, nullptr);
}

HRESULT STDAPICALLTYPE XGameSaveEnumerateContainerInfoByName(
    XGameSaveProviderHandle provider,
    const char* containerNamePrefix,
    void* context,
    XGameSaveContainerInfoCallback* callback) noexcept {
    if (!provider || !callback) return E_INVALIDARG;
    std::error_code ec;
    if (!fs::exists(provider->rootDir, ec)) return S_OK;

    const std::string prefix = containerNamePrefix ? containerNamePrefix : "";
    for (const auto& entry : fs::directory_iterator(provider->rootDir, ec)) {
        if (!entry.is_directory(ec)) continue;
        const std::string cname = entry.path().filename().string();
        if (!prefix.empty() && cname.rfind(prefix, 0) != 0) continue;

        uint32_t blobCount = 0;
        uint64_t totalSize = 0;
        for (const auto& bentry : fs::directory_iterator(entry.path(), ec)) {
            if (!bentry.is_regular_file(ec)) continue;
            if (bentry.path().filename().string() == kMetaFileName) continue;
            ++blobCount;
            totalSize += bentry.file_size(ec);
        }
        const std::string dname = ReadContainerDisplayName(entry.path(), cname);

        XGameSaveContainerInfo info{};
        info.name = cname.c_str();
        info.displayName = dname.c_str();
        info.blobCount = blobCount;
        info.totalSize = totalSize;
        info.lastModifiedTime = std::time(nullptr);
        info.needsSync = false;

        if (!callback(&info, context)) {
            break;
        }
    }
    return S_OK;
}

HRESULT STDAPICALLTYPE XGameSaveEnumerateContainerInfo(
    XGameSaveProviderHandle provider,
    void* context,
    XGameSaveContainerInfoCallback* callback) noexcept {
    return XGameSaveEnumerateContainerInfoByName(provider, nullptr, context, callback);
}

HRESULT STDAPICALLTYPE XGameSaveGetContainerInfo(
    XGameSaveProviderHandle provider,
    const char* containerName,
    void* context,
    XGameSaveContainerInfoCallback* callback) noexcept {
    if (!provider || !containerName || !containerName[0] || !callback) return E_INVALIDARG;
    const fs::path cdir = provider->rootDir / SanitizePathComponent(containerName);
    std::error_code ec;
    if (!fs::is_directory(cdir, ec)) return E_GS_BLOB_NOT_FOUND;

    uint32_t blobCount = 0;
    uint64_t totalSize = 0;
    for (const auto& bentry : fs::directory_iterator(cdir, ec)) {
        if (!bentry.is_regular_file(ec)) continue;
        if (bentry.path().filename().string() == kMetaFileName) continue;
        ++blobCount;
        totalSize += bentry.file_size(ec);
    }
    const std::string dname = ReadContainerDisplayName(cdir, containerName);
    XGameSaveContainerInfo info{};
    info.name = containerName;
    info.displayName = dname.c_str();
    info.blobCount = blobCount;
    info.totalSize = totalSize;
    info.lastModifiedTime = std::time(nullptr);
    info.needsSync = false;
    callback(&info, context);
    return S_OK;
}

HRESULT STDAPICALLTYPE XGameSaveEnumerateBlobInfoByName(
    XGameSaveContainerHandle container,
    const char* blobName,
    void* context,
    XGameSaveBlobInfoCallback* callback) noexcept {
    if (!container || !callback) return E_INVALIDARG;
    std::error_code ec;
    if (!fs::exists(container->containerDir, ec)) return S_OK;

    const std::string prefix = blobName ? blobName : "";
    for (const auto& entry : fs::directory_iterator(container->containerDir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        const std::string bname = entry.path().filename().string();
        if (bname == kMetaFileName) continue;
        if (!prefix.empty() && bname.rfind(prefix, 0) != 0) continue;

        XGameSaveBlobInfo info{};
        info.name = bname.c_str();
        info.size = static_cast<uint32_t>(entry.file_size(ec));
        if (!callback(&info, context)) {
            break;
        }
    }
    return S_OK;
}

HRESULT STDAPICALLTYPE XGameSaveEnumerateBlobInfo(
    XGameSaveContainerHandle container,
    void* context,
    XGameSaveBlobInfoCallback* callback) noexcept {
    return XGameSaveEnumerateBlobInfoByName(container, nullptr, context, callback);
}

namespace {

struct LoadedBlobRecord {
    std::string name;
    std::vector<uint8_t> bytes;
};

HRESULT LoadContainerBlobsFromDisk(
    XGameSaveContainerHandle container,
    const std::vector<std::string>& requestedNames,
    std::vector<LoadedBlobRecord>& outRecords,
    size_t& outRequiredBytes) {
    outRecords.clear();
    outRequiredBytes = 0;
    std::error_code ec;
    if (!fs::exists(container->containerDir, ec)) {
        return E_GS_BLOB_NOT_FOUND;
    }

    if (!requestedNames.empty()) {
        for (const auto& name : requestedNames) {
            const fs::path p = container->containerDir / SanitizePathComponent(name);
            if (!fs::is_regular_file(p, ec)) {
                return E_GS_BLOB_NOT_FOUND;
            }
            std::ifstream in(p, std::ios::binary | std::ios::ate);
            if (!in) return E_GS_BLOB_NOT_FOUND;
            const auto sz = static_cast<size_t>(in.tellg());
            in.seekg(0, std::ios::beg);
            LoadedBlobRecord rec{};
            rec.name = name;
            rec.bytes.resize(sz);
            if (sz > 0) {
                in.read(reinterpret_cast<char*>(rec.bytes.data()), static_cast<std::streamsize>(sz));
            }
            outRecords.push_back(std::move(rec));
        }
    } else {
        for (const auto& entry : fs::directory_iterator(container->containerDir, ec)) {
            if (!entry.is_regular_file(ec)) continue;
            const std::string bname = entry.path().filename().string();
            if (bname == kMetaFileName) continue;
            std::ifstream in(entry.path(), std::ios::binary | std::ios::ate);
            if (!in) continue;
            const auto sz = static_cast<size_t>(in.tellg());
            in.seekg(0, std::ios::beg);
            LoadedBlobRecord rec{};
            rec.name = bname;
            rec.bytes.resize(sz);
            if (sz > 0) {
                in.read(reinterpret_cast<char*>(rec.bytes.data()), static_cast<std::streamsize>(sz));
            }
            outRecords.push_back(std::move(rec));
        }
    }

    if (outRecords.empty()) {
        return E_GS_BLOB_NOT_FOUND;
    }

    size_t total = outRecords.size() * sizeof(XGameSaveBlob);
    for (const auto& rec : outRecords) {
        total += (rec.name.size() + 1) + rec.bytes.size();
    }
    outRequiredBytes = total;
    return S_OK;
}

HRESULT PackBlobsIntoUserBuffer(
    const std::vector<LoadedBlobRecord>& records,
    size_t requiredBytes,
    size_t blobsSize,
    XGameSaveBlob* blobData,
    uint32_t* countOfBlobs) {
    if (!blobData || blobsSize < requiredBytes) {
        return E_GS_PROVIDED_BUFFER_TOO_SMALL;
    }
    if (countOfBlobs) {
        *countOfBlobs = static_cast<uint32_t>(records.size());
    }

    uint8_t* tail = reinterpret_cast<uint8_t*>(blobData + records.size());
    for (size_t i = 0; i < records.size(); ++i) {
        const auto& rec = records[i];
        char* nameDst = reinterpret_cast<char*>(tail);
        std::memcpy(nameDst, rec.name.c_str(), rec.name.size() + 1);
        tail += rec.name.size() + 1;

        uint8_t* dataDst = tail;
        if (!rec.bytes.empty()) {
            std::memcpy(dataDst, rec.bytes.data(), rec.bytes.size());
            tail += rec.bytes.size();
        }

        blobData[i].info.name = nameDst;
        blobData[i].info.size = static_cast<uint32_t>(rec.bytes.size());
        blobData[i].data = dataDst;
    }
    return S_OK;
}

} // namespace

HRESULT STDAPICALLTYPE XGameSaveReadBlobData(
    XGameSaveContainerHandle container,
    const char* const* blobNames,
    uint32_t* countOfBlobs,
    size_t blobsSize,
    XGameSaveBlob* blobData) noexcept {
    if (!container || !countOfBlobs || !blobData) return E_INVALIDARG;

    std::vector<std::string> reqNames;
    if (blobNames && *countOfBlobs > 0) {
        for (uint32_t i = 0; i < *countOfBlobs; ++i) {
            if (blobNames[i]) reqNames.emplace_back(blobNames[i]);
        }
    }

    std::vector<LoadedBlobRecord> records;
    size_t requiredBytes = 0;
    const HRESULT hr = LoadContainerBlobsFromDisk(container, reqNames, records, requiredBytes);
    if (FAILED(hr)) return hr;

    return PackBlobsIntoUserBuffer(records, requiredBytes, blobsSize, blobData, countOfBlobs);
}

struct ReadBlobDataAsyncArgs {
    XGameSaveContainerHandle container = nullptr;
    std::vector<std::string> requestedNames;
    std::vector<LoadedBlobRecord> records;
    size_t requiredBytes = 0;
};

HRESULT STDAPICALLTYPE XGameSaveReadBlobDataAsync(
    XGameSaveContainerHandle container,
    const char* const* blobNames,
    uint32_t countOfBlobs,
    XAsyncBlock* async) noexcept {
    if (!container || !async) return E_INVALIDARG;

    auto* args = new (std::nothrow) ReadBlobDataAsyncArgs();
    if (!args) return E_OUTOFMEMORY;
    args->container = container;
    if (blobNames && countOfBlobs > 0) {
        for (uint32_t i = 0; i < countOfBlobs; ++i) {
            if (blobNames[i]) args->requestedNames.emplace_back(blobNames[i]);
        }
    }

    const HRESULT hr = XAsyncBegin(
        async, args, reinterpret_cast<const void*>(&XGameSaveReadBlobDataAsync),
        "XGameSaveReadBlobDataAsync",
        [](XAsyncOp op, const XAsyncProviderData* data) noexcept -> HRESULT {
            auto* a = static_cast<ReadBlobDataAsyncArgs*>(data->context);
            if (op == XAsyncOp::DoWork) {
                const HRESULT loadHr = LoadContainerBlobsFromDisk(
                    a->container, a->requestedNames, a->records, a->requiredBytes);
                XAsyncComplete(data->async, loadHr, a->requiredBytes);
                return S_OK;
            }
            if (op == XAsyncOp::GetResult) {
                uint32_t count = static_cast<uint32_t>(a->records.size());
                return PackBlobsIntoUserBuffer(
                    a->records, a->requiredBytes, data->bufferSize,
                    static_cast<XGameSaveBlob*>(data->buffer), &count);
            }
            if (op == XAsyncOp::Cleanup) {
                delete a;
            }
            return S_OK;
        });
    if (FAILED(hr)) {
        delete args;
        return hr;
    }
    return wsx::trace_hr(kMod, "XGameSaveReadBlobDataAsync", XAsyncSchedule(async, 0),
                         "container=%s count=%u", container->name.c_str(), countOfBlobs);
}

HRESULT STDAPICALLTYPE XGameSaveReadBlobDataResult(
    XAsyncBlock* async,
    size_t blobsSize,
    XGameSaveBlob* blobData,
    uint32_t* countOfBlobs) noexcept {
    if (!async || !blobData || !countOfBlobs) return E_INVALIDARG;
    *countOfBlobs = 0;

    WsxAsyncState* state = GetAsyncState(async);
    if (!state) return E_INVALIDARG;

    auto* a = static_cast<ReadBlobDataAsyncArgs*>(state->providerContext);
    const uint32_t recordCount = a ? static_cast<uint32_t>(a->records.size()) : 0u;
    const HRESULT hr = XAsyncGetResult(
        async, reinterpret_cast<const void*>(&XGameSaveReadBlobDataAsync),
        blobsSize, blobData, nullptr);
    if (SUCCEEDED(hr)) {
        *countOfBlobs = recordCount;
    }
    return wsx::trace_hr(kMod, "XGameSaveReadBlobDataResult", hr, "blobs=%u bytes=%zu", *countOfBlobs, blobsSize);
}

HRESULT STDAPICALLTYPE XGameSaveFilesGetFolderWithUiAsync(
    XUserHandle requestingUser,
    const char* configurationId,
    XAsyncBlock* async) noexcept {
    if (!async) return E_INVALIDARG;
    XGameSaveProviderHandle prov = nullptr;
    const HRESULT initHr = XGameSaveInitializeProvider(requestingUser, configurationId, false, &prov);
    if (FAILED(initHr)) return initHr;
    auto* folderStr = new (std::nothrow) std::string(prov->rootDir.string());
    XGameSaveCloseProvider(prov);
    if (!folderStr) return E_OUTOFMEMORY;

    const HRESULT hr = XAsyncBegin(
        async, folderStr, reinterpret_cast<const void*>(&XGameSaveFilesGetFolderWithUiAsync),
        "XGameSaveFilesGetFolderWithUiAsync",
        [](XAsyncOp op, const XAsyncProviderData* data) noexcept -> HRESULT {
            auto* s = static_cast<std::string*>(data->context);
            if (op == XAsyncOp::DoWork) {
                XAsyncComplete(data->async, S_OK, s->size() + 1);
                return S_OK;
            }
            if (op == XAsyncOp::GetResult) {
                if (!data->buffer || data->bufferSize < s->size() + 1) {
                    return E_NOT_SUFFICIENT_BUFFER;
                }
                std::memcpy(data->buffer, s->c_str(), s->size() + 1);
                return S_OK;
            }
            if (op == XAsyncOp::Cleanup) {
                delete s;
            }
            return S_OK;
        });
    if (FAILED(hr)) {
        delete folderStr;
        return hr;
    }
    return XAsyncSchedule(async, 0);
}

HRESULT STDAPICALLTYPE XGameSaveFilesGetFolderWithUiResultSize(
    XAsyncBlock* async,
    size_t* folderSize) noexcept {
    return XAsyncGetResultSize(async, folderSize);
}

HRESULT STDAPICALLTYPE XGameSaveFilesGetFolderWithUiResult(
    XAsyncBlock* async,
    size_t folderSize,
    char* folderResult) noexcept {
    return XAsyncGetResult(
        async, reinterpret_cast<const void*>(&XGameSaveFilesGetFolderWithUiAsync),
        folderSize, folderResult, nullptr);
}

HRESULT STDAPICALLTYPE XGameSaveFilesGetRemainingQuota(
    XUserHandle userContext,
    const char* configurationId,
    int64_t* remainingQuota) noexcept {
    if (!remainingQuota) return E_INVALIDARG;
    XGameSaveProviderHandle prov = nullptr;
    const HRESULT hr = XGameSaveInitializeProvider(userContext, configurationId, false, &prov);
    if (FAILED(hr)) return hr;
    const HRESULT qhr = XGameSaveGetRemainingQuota(prov, remainingQuota);
    XGameSaveCloseProvider(prov);
    return qhr;
}

// ----------------------------------------------------------------------------
// XNetworking Exports
// ----------------------------------------------------------------------------

HRESULT STDAPICALLTYPE XNetworkingGetConnectivityHint(
    XNetworkingConnectivityHint* connectivityHint) noexcept {
    if (!connectivityHint) return E_INVALIDARG;
    const bool offline = IsOfflineNetModeEnabled();
    connectivityHint->connectivityLevel = offline
        ? XNetworkingConnectivityLevelHint::LocalAccess
        : XNetworkingConnectivityLevelHint::InternetAccess;
    connectivityHint->connectivityCost = XNetworkingConnectivityCostHint::Unrestricted;
    connectivityHint->ianaInterfaceType = 6u; // IF_TYPE_ETHERNET_CSMACD (Wired)
    connectivityHint->networkInitialized = true;
    connectivityHint->approachingDataLimit = false;
    connectivityHint->overDataLimit = false;
    connectivityHint->roaming = false;
    return wsx::trace_hr(kMod, "XNetworkingGetConnectivityHint", S_OK, "level=%s wired=1",
                         offline ? "LocalAccess" : "InternetAccess");
}

HRESULT STDAPICALLTYPE XNetworkingRegisterConnectivityHintChanged(
    XTaskQueueHandle queue,
    void* context,
    XNetworkingConnectivityHintChangedCallback* callback,
    XTaskQueueRegistrationToken* token) noexcept {
    if (!callback || !token) return E_INVALIDARG;
    const uint64_t t = g_next_token.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(g_net_mtx);
        g_net_change_regs.push_back(NetChangeReg{t, queue, context, callback});
    }
    token->token = t;
    // Deliver initial connectivity hint notification asynchronously on the caller's queue
    struct InitHintCtx {
        XNetworkingConnectivityHintChangedCallback* cb;
        void* user_ctx;
    };
    auto* ic = new (std::nothrow) InitHintCtx{callback, context};
    if (ic) {
        const HRESULT shr = XTaskQueueSubmitCallback(
            queue, XTaskQueuePort::Completion, ic,
            [](void* p, bool canceled) noexcept {
                auto* c = static_cast<InitHintCtx*>(p);
                if (!canceled && c && c->cb) {
                    XNetworkingConnectivityHint hint{};
                    XNetworkingGetConnectivityHint(&hint);
                    c->cb(c->user_ctx, &hint);
                }
                delete c;
            });
        if (FAILED(shr)) {
            delete ic;
        }
    }
    return wsx::trace_hr(kMod, "XNetworkingRegisterConnectivityHintChanged", S_OK, "token=%llu",
                         static_cast<unsigned long long>(t));
}

bool STDAPICALLTYPE XNetworkingUnregisterConnectivityHintChanged(
    XTaskQueueRegistrationToken token,
    bool /*wait*/) noexcept {
    if (token.token == 0) return false;
    std::lock_guard<std::mutex> lock(g_net_mtx);
    auto it = std::remove_if(g_net_change_regs.begin(), g_net_change_regs.end(),
                             [&](const NetChangeReg& r) { return r.token == token.token; });
    if (it == g_net_change_regs.end()) return false;
    g_net_change_regs.erase(it, g_net_change_regs.end());
    return true;
}

HRESULT STDAPICALLTYPE XNetworkingQueryPreferredLocalUdpMultiplayerPort(
    uint16_t* preferredLocalUdpMultiplayerPort) noexcept {
    if (!preferredLocalUdpMultiplayerPort) return E_INVALIDARG;
    *preferredLocalUdpMultiplayerPort = 3074u;
    return wsx::trace_hr(kMod, "XNetworkingQueryPreferredLocalUdpMultiplayerPort", S_OK, "port=3074");
}

HRESULT STDAPICALLTYPE XNetworkingQueryPreferredLocalUdpMultiplayerPortAsync(
    XAsyncBlock* asyncBlock) noexcept {
    if (!asyncBlock) return E_INVALIDARG;
    const HRESULT hr = XAsyncBegin(
        asyncBlock, nullptr,
        reinterpret_cast<const void*>(&XNetworkingQueryPreferredLocalUdpMultiplayerPortAsync),
        "XNetworkingQueryPreferredLocalUdpMultiplayerPortAsync",
        [](XAsyncOp op, const XAsyncProviderData* data) noexcept -> HRESULT {
            if (op == XAsyncOp::DoWork) {
                XAsyncComplete(data->async, S_OK, sizeof(uint16_t));
                return S_OK;
            }
            if (op == XAsyncOp::GetResult) {
                *static_cast<uint16_t*>(data->buffer) = 3074u;
                return S_OK;
            }
            return S_OK;
        });
    if (FAILED(hr)) return hr;
    return XAsyncSchedule(asyncBlock, 0);
}

HRESULT STDAPICALLTYPE XNetworkingQueryPreferredLocalUdpMultiplayerPortResult(
    XAsyncBlock* asyncBlock,
    uint16_t* preferredLocalUdpMultiplayerPort) noexcept {
    if (!asyncBlock || !preferredLocalUdpMultiplayerPort) return E_INVALIDARG;
    return XAsyncGetResult(
        asyncBlock,
        reinterpret_cast<const void*>(&XNetworkingQueryPreferredLocalUdpMultiplayerPortAsync),
        sizeof(uint16_t), preferredLocalUdpMultiplayerPort, nullptr);
}

HRESULT STDAPICALLTYPE XNetworkingRegisterPreferredLocalUdpMultiplayerPortChanged(
    XTaskQueueHandle queue,
    void* context,
    XNetworkingPreferredLocalUdpMultiplayerPortChangedCallback* callback,
    XTaskQueueRegistrationToken* token) noexcept {
    if (!callback || !token) return E_INVALIDARG;
    const uint64_t t = g_next_token.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(g_net_mtx);
        g_net_udp_port_regs.push_back(NetUdpPortReg{t, queue, context, callback});
    }
    token->token = t;

    struct InitPortCtx {
        XNetworkingPreferredLocalUdpMultiplayerPortChangedCallback* cb;
        void* user_ctx;
    };
    auto* ic = new (std::nothrow) InitPortCtx{callback, context};
    if (ic) {
        const HRESULT shr = XTaskQueueSubmitCallback(
            queue, XTaskQueuePort::Completion, ic,
            [](void* p, bool canceled) noexcept {
                auto* c = static_cast<InitPortCtx*>(p);
                if (!canceled && c && c->cb) {
                    c->cb(c->user_ctx, 3074u);
                }
                delete c;
            });
        if (FAILED(shr)) delete ic;
    }
    return wsx::trace_hr(kMod, "XNetworkingRegisterPreferredLocalUdpMultiplayerPortChanged", S_OK,
                         "token=%llu", static_cast<unsigned long long>(t));
}

bool STDAPICALLTYPE XNetworkingUnregisterPreferredLocalUdpMultiplayerPortChanged(
    XTaskQueueRegistrationToken token,
    bool /*wait*/) noexcept {
    if (token.token == 0) return false;
    std::lock_guard<std::mutex> lock(g_net_mtx);
    auto it = std::remove_if(g_net_udp_port_regs.begin(), g_net_udp_port_regs.end(),
                             [&](const NetUdpPortReg& r) { return r.token == token.token; });
    if (it == g_net_udp_port_regs.end()) return false;
    g_net_udp_port_regs.erase(it, g_net_udp_port_regs.end());
    return true;
}

HRESULT STDAPICALLTYPE XNetworkingQuerySecurityInformationForUrlAsync(
    const char* url,
    XAsyncBlock* asyncBlock) noexcept {
    if (!url || !*url || !asyncBlock) return E_INVALIDARG;
    const HRESULT hr = XAsyncBegin(
        asyncBlock, nullptr,
        reinterpret_cast<const void*>(&XNetworkingQuerySecurityInformationForUrlAsync),
        "XNetworkingQuerySecurityInformationForUrlAsync",
        [](XAsyncOp op, const XAsyncProviderData* data) noexcept -> HRESULT {
            if (op == XAsyncOp::DoWork) {
                XAsyncComplete(data->async, S_OK, sizeof(XNetworkingSecurityInformation));
                return S_OK;
            }
            if (op == XAsyncOp::GetResult) {
                if (!data->buffer || data->bufferSize < sizeof(XNetworkingSecurityInformation)) {
                    return E_NOT_SUFFICIENT_BUFFER;
                }
                auto* info = static_cast<XNetworkingSecurityInformation*>(data->buffer);
                info->enabledHttpSecurityProtocolFlags = 0u;
                info->thumbprintCount = 0u;
                info->thumbprints = nullptr;
                return S_OK;
            }
            return S_OK;
        });
    if (FAILED(hr)) return hr;
    return XAsyncSchedule(asyncBlock, 0);
}

HRESULT STDAPICALLTYPE XNetworkingQuerySecurityInformationForUrlResultSize(
    XAsyncBlock* asyncBlock,
    size_t* securityInformationBufferByteCount) noexcept {
    return XAsyncGetResultSize(asyncBlock, securityInformationBufferByteCount);
}

HRESULT STDAPICALLTYPE XNetworkingQuerySecurityInformationForUrlResult(
    XAsyncBlock* asyncBlock,
    size_t securityInformationBufferByteCount,
    size_t* securityInformationBufferByteCountUsed,
    uint8_t* securityInformationBuffer,
    XNetworkingSecurityInformation** securityInformation) noexcept {
    if (!securityInformationBuffer || !securityInformation) return E_INVALIDARG;
    *securityInformation = nullptr;
    const HRESULT hr = XAsyncGetResult(
        asyncBlock,
        reinterpret_cast<const void*>(&XNetworkingQuerySecurityInformationForUrlAsync),
        securityInformationBufferByteCount, securityInformationBuffer,
        securityInformationBufferByteCountUsed);
    if (SUCCEEDED(hr)) {
        *securityInformation = reinterpret_cast<XNetworkingSecurityInformation*>(securityInformationBuffer);
    }
    return hr;
}

HRESULT STDAPICALLTYPE XNetworkingQuerySecurityInformationForUrlUtf16Async(
    const wchar_t* url,
    XAsyncBlock* asyncBlock) noexcept {
    if (!url || !*url || !asyncBlock) return E_INVALIDARG;
    const HRESULT hr = XAsyncBegin(
        asyncBlock, nullptr,
        reinterpret_cast<const void*>(&XNetworkingQuerySecurityInformationForUrlUtf16Async),
        "XNetworkingQuerySecurityInformationForUrlUtf16Async",
        [](XAsyncOp op, const XAsyncProviderData* data) noexcept -> HRESULT {
            if (op == XAsyncOp::DoWork) {
                XAsyncComplete(data->async, S_OK, sizeof(XNetworkingSecurityInformation));
                return S_OK;
            }
            if (op == XAsyncOp::GetResult) {
                if (!data->buffer || data->bufferSize < sizeof(XNetworkingSecurityInformation)) {
                    return E_NOT_SUFFICIENT_BUFFER;
                }
                auto* info = static_cast<XNetworkingSecurityInformation*>(data->buffer);
                info->enabledHttpSecurityProtocolFlags = 0u;
                info->thumbprintCount = 0u;
                info->thumbprints = nullptr;
                return S_OK;
            }
            return S_OK;
        });
    if (FAILED(hr)) return hr;
    return XAsyncSchedule(asyncBlock, 0);
}

HRESULT STDAPICALLTYPE XNetworkingQuerySecurityInformationForUrlUtf16ResultSize(
    XAsyncBlock* asyncBlock,
    size_t* securityInformationBufferByteCount) noexcept {
    return XAsyncGetResultSize(asyncBlock, securityInformationBufferByteCount);
}

HRESULT STDAPICALLTYPE XNetworkingQuerySecurityInformationForUrlUtf16Result(
    XAsyncBlock* asyncBlock,
    size_t securityInformationBufferByteCount,
    size_t* securityInformationBufferByteCountUsed,
    uint8_t* securityInformationBuffer,
    XNetworkingSecurityInformation** securityInformation) noexcept {
    if (!securityInformationBuffer || !securityInformation) return E_INVALIDARG;
    *securityInformation = nullptr;
    const HRESULT hr = XAsyncGetResult(
        asyncBlock,
        reinterpret_cast<const void*>(&XNetworkingQuerySecurityInformationForUrlUtf16Async),
        securityInformationBufferByteCount, securityInformationBuffer,
        securityInformationBufferByteCountUsed);
    if (SUCCEEDED(hr)) {
        *securityInformation = reinterpret_cast<XNetworkingSecurityInformation*>(securityInformationBuffer);
    }
    return hr;
}

HRESULT STDAPICALLTYPE XNetworkingVerifyServerCertificate(
    void* requestHandle,
    const XNetworkingThumbprint* thumbprint) noexcept {
    if (thumbprint && thumbprint->thumbprintBufferByteCount > 0 && !thumbprint->thumbprintBuffer) {
        return E_INVALIDARG;
    }
    if (IsOfflineNetModeEnabled() || !requestHandle || !thumbprint || thumbprint->thumbprintBufferByteCount == 0) {
        return S_OK;
    }

    DWORD prop_id = 0;
    if (thumbprint->thumbprintBufferByteCount == 32u) {
        prop_id = 107u; // CERT_SHA256_HASH_PROP_ID
    } else if (thumbprint->thumbprintBufferByteCount == 20u) {
        prop_id = 3u;   // CERT_HASH_PROP_ID (SHA-1)
    } else {
        return E_INVALIDARG;
    }

    HMODULE winhttp = LoadLibraryW(L"winhttp.dll");
    HMODULE crypt32 = LoadLibraryW(L"crypt32.dll");
    if (!winhttp || !crypt32) {
        if (winhttp) FreeLibrary(winhttp);
        if (crypt32) FreeLibrary(crypt32);
        return E_FAIL;
    }

    using WinHttpQueryOptionFn = BOOL(WINAPI*)(void*, DWORD, void*, DWORD*);
    using CertGetPropFn = BOOL(WINAPI*)(const void*, DWORD, void*, DWORD*);
    using CertFreeCtxFn = BOOL(WINAPI*)(const void*);

    const auto pQueryOpt = reinterpret_cast<WinHttpQueryOptionFn>(GetProcAddress(winhttp, "WinHttpQueryOption"));
    const auto pGetProp = reinterpret_cast<CertGetPropFn>(GetProcAddress(crypt32, "CertGetCertificateContextProperty"));
    const auto pFreeCert = reinterpret_cast<CertFreeCtxFn>(GetProcAddress(crypt32, "CertFreeCertificateContext"));

    HRESULT hr = E_FAIL;
    if (pQueryOpt && pGetProp && pFreeCert) {
        constexpr DWORD kWinHttpOptionServerCertContext = 78u;
        const void* cert_ctx = nullptr;
        DWORD cert_ctx_sz = sizeof(cert_ctx);
        if (!pQueryOpt(requestHandle, kWinHttpOptionServerCertContext, &cert_ctx, &cert_ctx_sz) || !cert_ctx) {
            const DWORD err = GetLastError();
            hr = HRESULT_FROM_WIN32(err ? err : ERROR_INVALID_HANDLE);
        } else {
            uint8_t actual_hash[32]{};
            DWORD hash_sz = static_cast<DWORD>(thumbprint->thumbprintBufferByteCount);
            if (!pGetProp(cert_ctx, prop_id, actual_hash, &hash_sz) ||
                hash_sz != static_cast<DWORD>(thumbprint->thumbprintBufferByteCount)) {
                const DWORD err = GetLastError();
                hr = HRESULT_FROM_WIN32(err ? err : ERROR_INVALID_DATA);
            } else {
                uint8_t diff = 0u;
                for (size_t i = 0; i < thumbprint->thumbprintBufferByteCount; ++i) {
                    diff |= static_cast<uint8_t>(actual_hash[i] ^ thumbprint->thumbprintBuffer[i]);
                }
                hr = (diff == 0u) ? S_OK : static_cast<HRESULT>(0x80091007L); // CRYPT_E_HASH_VALUE
            }
            pFreeCert(cert_ctx);
        }
    }

    FreeLibrary(crypt32);
    FreeLibrary(winhttp);
    return wsx::trace_hr(kMod, "XNetworkingVerifyServerCertificate", hr, "len=%zu",
                         thumbprint->thumbprintBufferByteCount);
}

HRESULT STDAPICALLTYPE XNetworkingQueryStatistics(
    XNetworkingStatisticsType statisticsType,
    XNetworkingStatisticsBuffer* statisticsBuffer) noexcept {
    if (!statisticsBuffer) return E_INVALIDARG;
    std::memset(statisticsBuffer, 0, sizeof(*statisticsBuffer));
    if (statisticsType != XNetworkingStatisticsType::TitleTcpConnections &&
        statisticsType != XNetworkingStatisticsType::TitleUdpConnections) {
        return E_INVALIDARG;
    }
    return wsx::trace_hr(kMod, "XNetworkingQueryStatistics", S_OK, "type=%u",
                         static_cast<unsigned>(statisticsType));
}

HRESULT STDAPICALLTYPE XNetworkingQueryConfigurationSetting(
    XNetworkingConfigurationSetting configuration,
    uint64_t* value) noexcept {
    if (!value) return E_INVALIDARG;
    switch (configuration) {
        case XNetworkingConfigurationSetting::MaxTitleTcpConnections:
            *value = 256u;
            return S_OK;
        case XNetworkingConfigurationSetting::MaxTitleUdpConnections:
            *value = 128u;
            return S_OK;
        default:
            *value = 0u;
            return E_INVALIDARG;
    }
}

// ----------------------------------------------------------------------------
// XPersistentLocalStorage Exports
// ----------------------------------------------------------------------------

HRESULT STDAPICALLTYPE XPersistentLocalStorageGetPathSize(
    size_t* pathSize) noexcept {
    if (!pathSize) return E_INVALIDARG;
    const std::string p = GetPlsDirectory().string();
    *pathSize = p.size() + 1;
    return S_OK;
}

HRESULT STDAPICALLTYPE XPersistentLocalStorageGetPath(
    size_t pathSize,
    char* path,
    size_t* pathUsed) noexcept {
    const std::string p = GetPlsDirectory().string();
    const HRESULT hr = CopyStringToBuffer(p, pathSize, path, pathUsed);
    return wsx::trace_hr(kMod, "XPersistentLocalStorageGetPath", hr, "path=%s", p.c_str());
}

HRESULT STDAPICALLTYPE XPersistentLocalStorageGetSpaceInfo(
    XPersistentLocalStorageSpaceInfo* spaceInfo) noexcept {
    if (!spaceInfo) return E_INVALIDARG;
    const fs::path dir = GetPlsDirectory();
    const uint64_t used = ComputeDirectoryUsedBytes(dir);
    const uint64_t diskFree = QueryDiskFreeBytes(dir);
    const uint32_t plsMb = std::max<uint32_t>(64u, wsx_pkg::get_hydrated_config().pls_size_mb);
    const uint64_t kMaxPlsQuota = static_cast<uint64_t>(plsMb) * 1024ULL * 1024ULL;
    const uint64_t total = std::min<uint64_t>(kMaxPlsQuota, used + diskFree);
    const uint64_t free = (total > used) ? (total - used) : 0ULL;
    spaceInfo->usedBytes = used;
    spaceInfo->totalBytes = total;
    spaceInfo->availableFreeBytes = free;
    spaceInfo->totalFreeBytes = free;
    return wsx::trace_hr(kMod, "XPersistentLocalStorageGetSpaceInfo", S_OK,
                         "used=%llu free=%llu",
                         static_cast<unsigned long long>(used),
                         static_cast<unsigned long long>(free));
}

HRESULT STDAPICALLTYPE XPersistentLocalStoragePromptUserForSpaceAsync(
    uint64_t /*requestedBytes*/,
    XAsyncBlock* asyncBlock) noexcept {
    if (!asyncBlock) return E_INVALIDARG;
    const HRESULT hr = XAsyncBegin(
        asyncBlock, nullptr,
        reinterpret_cast<const void*>(&XPersistentLocalStoragePromptUserForSpaceAsync),
        "XPersistentLocalStoragePromptUserForSpaceAsync",
        [](XAsyncOp op, const XAsyncProviderData* data) noexcept -> HRESULT {
            if (op == XAsyncOp::DoWork) {
                XAsyncComplete(data->async, S_OK, 0);
            }
            return S_OK;
        });
    if (FAILED(hr)) return hr;
    return XAsyncSchedule(asyncBlock, 0);
}

HRESULT STDAPICALLTYPE XPersistentLocalStoragePromptUserForSpaceResult(
    XAsyncBlock* asyncBlock) noexcept {
    return XAsyncGetResult(
        asyncBlock,
        reinterpret_cast<const void*>(&XPersistentLocalStoragePromptUserForSpaceAsync),
        0, nullptr, nullptr);
}

// ----------------------------------------------------------------------------
// XPackage Exports
// ----------------------------------------------------------------------------

HRESULT STDAPICALLTYPE XPackageGetCurrentProcessPackageIdentifier(
    size_t bufferSize,
    char* buffer) noexcept {
    const ParsedGameIdentity id = ReadLocalGameConfigIdentity();
    const HRESULT hr = CopyStringToBuffer(id.packageId, bufferSize, buffer, nullptr);
    return wsx::trace_hr(kMod, "XPackageGetCurrentProcessPackageIdentifier", hr, "id=%s", id.packageId.c_str());
}

bool STDAPICALLTYPE XPackageIsPackagedProcess() noexcept {
    return true;
}

HRESULT STDAPICALLTYPE XPackageGetUserLocale(
    size_t localeSize,
    char* locale) noexcept {
    std::string loc = wsx::get_config_string("WSX_LOCALE", "Console", "locale", "");
    if (loc.empty()) {
        wchar_t wloc[LOCALE_NAME_MAX_LENGTH]{};
        if (GetUserDefaultLocaleName(wloc, LOCALE_NAME_MAX_LENGTH) > 0) {
            char utf8[LOCALE_NAME_MAX_BYTES]{};
            WideCharToMultiByte(CP_UTF8, 0, wloc, -1, utf8, sizeof(utf8), nullptr, nullptr);
            loc = utf8;
        } else {
            loc = "en-US";
        }
    }
    const HRESULT hr = CopyStringToBuffer(loc, localeSize, locale, nullptr);
    return wsx::trace_hr(kMod, "XPackageGetUserLocale", hr, "locale=%s", loc.c_str());
}

HRESULT STDAPICALLTYPE XPackageEnumeratePackages(
    XPackageKind kind,
    XPackageEnumerationScope /*scope*/,
    void* context,
    XPackageEnumerationCallback* callback) noexcept {
    if (!callback) return E_INVALIDARG;
    if (kind == XPackageKind::Game) {
        const ParsedGameIdentity id = ReadLocalGameConfigIdentity();
        XPackageDetails details{};
        details.packageIdentifier = id.packageId.c_str();
        details.version = id.version;
        details.kind = XPackageKind::Game;
        details.displayName = id.displayName.c_str();
        details.description = id.displayName.c_str();
        details.publisher = id.publisher.c_str();
        details.storeId = id.storeId.c_str();
        details.installing = false;
        details.index = 0;
        details.count = 1;
        details.ageRestricted = false;
        details.titleId = id.titleId.c_str();
        callback(context, &details);
    }
    return wsx::trace_hr(kMod, "XPackageEnumeratePackages", S_OK, "kind=%u", static_cast<unsigned>(kind));
}

HRESULT STDAPICALLTYPE XPackageCreateInstallationMonitor(
    const char* packageIdentifier,
    uint32_t /*selectorCount*/,
    const XPackageChunkSelector* /*selectors*/,
    uint32_t /*minimumUpdateIntervalMs*/,
    XTaskQueueHandle /*queue*/,
    XPackageInstallationMonitorHandle* installationMonitor) noexcept {
    if (!packageIdentifier || !installationMonitor) return E_INVALIDARG;
    auto* mon = new (std::nothrow) XPackageInstallationMonitor();
    if (!mon) return E_OUTOFMEMORY;
    mon->packageId = packageIdentifier;
    const auto& cfg = wsx_pkg::get_hydrated_config();
    mon->totalBytes = (cfg.total_installed_bytes > 0ULL) ? cfg.total_installed_bytes : (1024ULL * 1024ULL * 64ULL);
    *installationMonitor = mon;
    return wsx::trace_hr(kMod, "XPackageCreateInstallationMonitor", S_OK, "pkg=%s", packageIdentifier);
}

void STDAPICALLTYPE XPackageGetInstallationProgress(
    XPackageInstallationMonitorHandle installationMonitor,
    XPackageInstallationProgress* progress) noexcept {
    if (!progress) return;
    const uint64_t total = installationMonitor ? installationMonitor->totalBytes : (1024ULL * 1024ULL * 64ULL);
    progress->totalBytes = total;
    progress->installedBytes = total;
    progress->launchBytes = total;
    progress->launchable = true;
    progress->completed = true;
}

void STDAPICALLTYPE XPackageCloseInstallationMonitorHandle(
    XPackageInstallationMonitorHandle installationMonitor) noexcept {
    delete installationMonitor;
}

HRESULT STDAPICALLTYPE XPackageMountWithUiAsync(
    const char* packageIdentifier,
    XAsyncBlock* async) noexcept {
    if (!packageIdentifier || !async) return E_INVALIDARG;
    auto* pkgId = new (std::nothrow) std::string(packageIdentifier);
    if (!pkgId) return E_OUTOFMEMORY;

    const HRESULT hr = XAsyncBegin(
        async, pkgId, reinterpret_cast<const void*>(&XPackageMountWithUiAsync),
        "XPackageMountWithUiAsync",
        [](XAsyncOp op, const XAsyncProviderData* data) noexcept -> HRESULT {
            auto* id = static_cast<std::string*>(data->context);
            if (op == XAsyncOp::DoWork) {
                XAsyncComplete(data->async, S_OK, sizeof(XPackageMountHandle));
                return S_OK;
            }
            if (op == XAsyncOp::GetResult) {
                auto* m = new (std::nothrow) XPackageMount();
                if (!m) return E_OUTOFMEMORY;
                m->packageId = *id;
                wchar_t exePath[MAX_PATH]{};
                GetModuleFileNameW(nullptr, exePath, MAX_PATH);
                m->mountPath = fs::path(exePath).parent_path().string();
                *static_cast<XPackageMountHandle*>(data->buffer) = m;
                return S_OK;
            }
            if (op == XAsyncOp::Cleanup) {
                delete id;
            }
            return S_OK;
        });
    if (FAILED(hr)) {
        delete pkgId;
        return hr;
    }
    return wsx::trace_hr(kMod, "XPackageMountWithUiAsync", XAsyncSchedule(async, 0), "pkg=%s", packageIdentifier);
}

HRESULT STDAPICALLTYPE XPackageMountWithUiResult(
    XAsyncBlock* async,
    XPackageMountHandle* mount) noexcept {
    if (!async || !mount) return E_INVALIDARG;
    *mount = nullptr;
    return XAsyncGetResult(
        async, reinterpret_cast<const void*>(&XPackageMountWithUiAsync),
        sizeof(XPackageMountHandle), mount, nullptr);
}

HRESULT STDAPICALLTYPE XPackageGetMountPathSize(
    XPackageMountHandle mount,
    size_t* pathSize) noexcept {
    if (!mount || !pathSize) return E_INVALIDARG;
    *pathSize = mount->mountPath.size() + 1;
    return S_OK;
}

HRESULT STDAPICALLTYPE XPackageGetMountPath(
    XPackageMountHandle mount,
    size_t pathSize,
    char* path) noexcept {
    if (!mount) return E_INVALIDARG;
    return CopyStringToBuffer(mount->mountPath, pathSize, path, nullptr);
}

void STDAPICALLTYPE XPackageCloseMountHandle(
    XPackageMountHandle mount) noexcept {
    delete mount;
}

HRESULT STDAPICALLTYPE XPackageFindChunkAvailability(
    const char* /*packageIdentifier*/,
    uint32_t selectorCount,
    const XPackageChunkSelector* selectors,
    XPackageChunkAvailability* availability) noexcept {
    if (!availability) return E_INVALIDARG;
    *availability = XPackageChunkAvailability::Ready;
    const auto& cfg = wsx_pkg::get_hydrated_config();
    if (selectors && selectorCount > 0u && !cfg.chunks.empty()) {
        for (uint32_t i = 0; i < selectorCount; ++i) {
            if (selectors[i].type == XPackageChunkSelectorType::Chunk) {
                for (const auto& ch : cfg.chunks) {
                    if (ch.chunk_id == selectors[i].chunkId && !ch.all_present) {
                        *availability = XPackageChunkAvailability::Pending;
                        return S_OK;
                    }
                }
            }
        }
    }
    return S_OK;
}

HRESULT STDAPICALLTYPE XPackageEnumerateChunkAvailability(
    const char* /*packageIdentifier*/,
    XPackageChunkSelectorType type,
    void* context,
    XPackageChunkAvailabilityCallback* callback) noexcept {
    if (!callback) return E_INVALIDARG;
    const auto& cfg = wsx_pkg::get_hydrated_config();
    if (type == XPackageChunkSelectorType::Chunk && !cfg.chunks.empty()) {
        for (const auto& ch : cfg.chunks) {
            XPackageChunkSelector sel{};
            sel.type = XPackageChunkSelectorType::Chunk;
            sel.chunkId = ch.chunk_id;
            const auto avail = ch.all_present
                ? XPackageChunkAvailability::Ready
                : XPackageChunkAvailability::Pending;
            if (!callback(context, &sel, avail)) break;
        }
        return S_OK;
    }
    if (type == XPackageChunkSelectorType::Language && !cfg.locales.empty()) {
        for (const auto& lang : cfg.locales) {
            XPackageChunkSelector sel{};
            sel.type = XPackageChunkSelectorType::Language;
            sel.language = lang.c_str();
            if (!callback(context, &sel, XPackageChunkAvailability::Ready)) break;
        }
        return S_OK;
    }
    XPackageChunkSelector sel{};
    sel.type = type;
    sel.chunkId = 0;
    callback(context, &sel, XPackageChunkAvailability::Ready);
    return S_OK;
}

} // extern "C"

// ----------------------------------------------------------------------------
// GDK Static Thunk Loader ABI (used by XCurl.dll, libHttpClient.GDK.dll, GDK titles)
// ----------------------------------------------------------------------------

namespace {

// {073B7DCB-1FCF-4030-94BE-E3C9EB623428} — IXAsync / IXTaskQueue internal GDK API table
constexpr GUID kGuidAsyncTaskQueueApi = {
    0x073B7DCBu, 0x1FCFu, 0x4030u, {0x94u, 0xBEu, 0xE3u, 0xC9u, 0xEBu, 0x62u, 0x34u, 0x28u}
};

// {8CA467F7-22E8-4096-8456-BB8AA13F79D8} — IXError internal GDK API table (XErrorReport)
constexpr GUID kGuidErrorApi = {
    0x8CA467F7u, 0x22E8u, 0x4096u, {0x84u, 0x56u, 0xBBu, 0x8Au, 0xA1u, 0x3Fu, 0x79u, 0xD8u}
};

// {37E56907-2F10-41E8-B72F-36EDB185331A} / {BF2346B2-39AF-4658-B5EA-44713C7E83B3} — IXNetworking internal GDK API table
constexpr GUID kGuidNetworkingClsid = {
    0x37E56907u, 0x2F10u, 0x41E8u, {0xB7u, 0x2Fu, 0x36u, 0xEDu, 0xB1u, 0x85u, 0x33u, 0x1Au}
};
constexpr GUID kGuidNetworkingIid = {
    0xBF2346B2u, 0x39AFu, 0x4658u, {0xB5u, 0xEAu, 0x44u, 0x71u, 0x3Cu, 0x7Eu, 0x83u, 0xB3u}
};

// Forward decls of XAsyncBeginAlloc helper (same as XAsyncBegin with heap context allocation)
struct WsxAsyncTaskQueueComImpl {
    void** vtable;
    std::atomic<ULONG> ref{1};
};

HRESULT STDMETHODCALLTYPE atq_qi(WsxAsyncTaskQueueComImpl* self, REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == kGuidAsyncTaskQueueApi) {
        *ppv = self;
        self->ref.fetch_add(1);
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}
ULONG STDMETHODCALLTYPE atq_addref(WsxAsyncTaskQueueComImpl* self) { return self->ref.fetch_add(1) + 1; }
ULONG STDMETHODCALLTYPE atq_release(WsxAsyncTaskQueueComImpl* self) { return self->ref.fetch_sub(1) - 1; }

HRESULT STDMETHODCALLTYPE atq_get_status(WsxAsyncTaskQueueComImpl*, XAsyncBlock* ab, bool wait) {
    return XAsyncGetStatus(ab, wait);
}
HRESULT STDMETHODCALLTYPE atq_get_result_size(WsxAsyncTaskQueueComImpl*, XAsyncBlock* ab, size_t* sz) {
    return XAsyncGetResultSize(ab, sz);
}
void STDMETHODCALLTYPE atq_cancel(WsxAsyncTaskQueueComImpl*, XAsyncBlock* ab) {
    XAsyncCancel(ab);
}
HRESULT STDMETHODCALLTYPE atq_run(WsxAsyncTaskQueueComImpl*, XAsyncBlock* ab, XAsyncWork* work) {
    return XAsyncRun(ab, work);
}
HRESULT STDMETHODCALLTYPE atq_begin(WsxAsyncTaskQueueComImpl*, XAsyncBlock* ab, void* ctx, const void* id, const char* name, XAsyncProvider* prov) {
    return XAsyncBegin(ab, ctx, id, name, prov);
}
HRESULT STDMETHODCALLTYPE atq_begin_alloc(WsxAsyncTaskQueueComImpl*, XAsyncBlock* ab, const void* id, const char* name, XAsyncProvider* prov, size_t ctx_size) {
    void* mem = ctx_size ? std::calloc(1, ctx_size) : nullptr;
    return XAsyncBeginInternal(ab, mem, mem != nullptr, id, name, prov);
}
HRESULT STDMETHODCALLTYPE atq_schedule(WsxAsyncTaskQueueComImpl*, XAsyncBlock* ab, uint32_t delayMs) {
    return XAsyncSchedule(ab, delayMs);
}
void STDMETHODCALLTYPE atq_complete(WsxAsyncTaskQueueComImpl*, XAsyncBlock* ab, HRESULT hr, size_t resSz) {
    XAsyncComplete(ab, hr, resSz);
}
HRESULT STDMETHODCALLTYPE atq_get_result(WsxAsyncTaskQueueComImpl*, XAsyncBlock* ab, const void* id, size_t bufSz, void* buf, size_t* used) {
    return XAsyncGetResult(ab, id, bufSz, buf, used);
}
HRESULT STDMETHODCALLTYPE atq_tq_create(WsxAsyncTaskQueueComImpl*, XTaskQueueDispatchMode w, XTaskQueueDispatchMode c, XTaskQueueHandle* out) {
    return XTaskQueueCreate(w, c, out);
}
HRESULT STDMETHODCALLTYPE atq_tq_create_composite(WsxAsyncTaskQueueComImpl*, void* wp, void* cp, XTaskQueueHandle* out) {
    return XTaskQueueCreateComposite(static_cast<XTaskQueuePortHandle>(wp),
                                     static_cast<XTaskQueuePortHandle>(cp), out);
}
HRESULT STDMETHODCALLTYPE atq_tq_get_port(WsxAsyncTaskQueueComImpl*, XTaskQueueHandle q, XTaskQueuePort p, void** outPort) {
    return XTaskQueueGetPort(q, p, reinterpret_cast<XTaskQueuePortHandle*>(outPort));
}
HRESULT STDMETHODCALLTYPE atq_tq_dup(WsxAsyncTaskQueueComImpl*, XTaskQueueHandle q, XTaskQueueHandle* out) {
    return XTaskQueueDuplicateHandle(q, out);
}
bool STDMETHODCALLTYPE atq_tq_dispatch(WsxAsyncTaskQueueComImpl*, XTaskQueueHandle q, XTaskQueuePort p, uint32_t ms) {
    return XTaskQueueDispatch(q, p, ms);
}
void STDMETHODCALLTYPE atq_tq_close(WsxAsyncTaskQueueComImpl*, XTaskQueueHandle q) {
    XTaskQueueCloseHandle(q);
}
HRESULT STDMETHODCALLTYPE atq_tq_submit(WsxAsyncTaskQueueComImpl*, XTaskQueueHandle q, XTaskQueuePort p, void* ctx, XTaskQueueCallback* cb) {
    return XTaskQueueSubmitCallback(q, p, ctx, cb);
}
HRESULT STDMETHODCALLTYPE atq_tq_submit_delayed(WsxAsyncTaskQueueComImpl*, XTaskQueueHandle q, XTaskQueuePort p, uint32_t ms, void* ctx, XTaskQueueCallback* cb) {
    return XTaskQueueSubmitDelayedCallback(q, p, ms, ctx, cb);
}
HRESULT STDMETHODCALLTYPE atq_tq_reg_mon(WsxAsyncTaskQueueComImpl*, XTaskQueueHandle q, void* ctx, void* cb, XTaskQueueRegistrationToken* tok) {
    return XTaskQueueRegisterMonitor(q, ctx, reinterpret_cast<XTaskQueueMonitorCallback*>(cb), tok);
}
void STDMETHODCALLTYPE atq_tq_unreg_mon(WsxAsyncTaskQueueComImpl*, XTaskQueueHandle q, XTaskQueueRegistrationToken tok) {
    XTaskQueueUnregisterMonitor(q, tok);
}
HRESULT STDMETHODCALLTYPE atq_tq_terminate(WsxAsyncTaskQueueComImpl*, XTaskQueueHandle q, bool wait, void* ctx, XTaskQueueTerminatedCallback* cb) {
    return XTaskQueueTerminate(q, wait, ctx, cb);
}
bool STDMETHODCALLTYPE atq_tq_get_proc(WsxAsyncTaskQueueComImpl*, XTaskQueueHandle* out) {
    return XTaskQueueGetCurrentProcessTaskQueue(out);
}
void STDMETHODCALLTYPE atq_tq_set_proc(WsxAsyncTaskQueueComImpl*, XTaskQueueHandle q) {
    XTaskQueueSetCurrentProcessTaskQueue(q);
}
HRESULT STDMETHODCALLTYPE atq_thread_set_ts(WsxAsyncTaskQueueComImpl*, bool isTimeSensitive) {
    return XThreadSetTimeSensitive(isTimeSensitive);
}
bool STDMETHODCALLTYPE atq_thread_is_ts(WsxAsyncTaskQueueComImpl*) {
    return XThreadIsTimeSensitive();
}
void STDMETHODCALLTYPE atq_assert_not_time_sensitive(WsxAsyncTaskQueueComImpl*) {
    XThreadAssertNotTimeSensitive();
}
template <uint32_t Slot>
HRESULT STDMETHODCALLTYPE atq_unhandled(WsxAsyncTaskQueueComImpl*) {
    if (GetEnvironmentVariableW(L"WSX_TRACE_API_IMPL", nullptr, 0) > 0) {
        std::fprintf(stderr, "      [xgameruntime::IXAsyncTaskQueue] unhandled slot %u (0x%02X)\n", Slot, Slot * 8);
    }
    return E_NOTIMPL;
}

void* g_atq_vtable[32] = {
    reinterpret_cast<void*>(&atq_qi),                  // 0x00 (0)
    reinterpret_cast<void*>(&atq_addref),              // 0x08 (1)
    reinterpret_cast<void*>(&atq_release),             // 0x10 (2)
    reinterpret_cast<void*>(&atq_get_status),          // 0x18 (3)
    reinterpret_cast<void*>(&atq_get_result_size),     // 0x20 (4)
    reinterpret_cast<void*>(&atq_cancel),              // 0x28 (5)
    reinterpret_cast<void*>(&atq_run),                 // 0x30 (6)
    reinterpret_cast<void*>(&atq_begin),               // 0x38 (7)
    reinterpret_cast<void*>(&atq_begin_alloc),         // 0x40 (8)
    reinterpret_cast<void*>(&atq_schedule),            // 0x48 (9)
    reinterpret_cast<void*>(&atq_complete),            // 0x50 (10)
    reinterpret_cast<void*>(&atq_get_result),          // 0x58 (11)
    reinterpret_cast<void*>(&atq_tq_create),           // 0x60 (12)
    reinterpret_cast<void*>(&atq_tq_create_composite), // 0x68 (13)
    reinterpret_cast<void*>(&atq_tq_get_port),         // 0x70 (14)
    reinterpret_cast<void*>(&atq_tq_dup),              // 0x78 (15)
    reinterpret_cast<void*>(&atq_tq_dispatch),         // 0x80 (16)
    reinterpret_cast<void*>(&atq_tq_close),            // 0x88 (17)
    reinterpret_cast<void*>(&atq_tq_submit),           // 0x90 (18)
    reinterpret_cast<void*>(&atq_tq_submit_delayed),   // 0x98 (19)
    reinterpret_cast<void*>(&atq_tq_reg_mon),          // 0xA0 (20)
    reinterpret_cast<void*>(&atq_tq_unreg_mon),        // 0xA8 (21)
    reinterpret_cast<void*>(&atq_tq_terminate),        // 0xB0 (22)
    reinterpret_cast<void*>(&atq_tq_get_proc),         // 0xB8 (23)
    reinterpret_cast<void*>(&atq_tq_set_proc),         // 0xC0 (24)
    reinterpret_cast<void*>(&atq_tq_get_proc),         // 0xC8 (25) XTaskQueueGetCurrentProcessTaskQueue
    reinterpret_cast<void*>(&atq_tq_set_proc),         // 0xD0 (26) XTaskQueueSetCurrentProcessTaskQueue
    reinterpret_cast<void*>(&atq_thread_set_ts),       // 0xD8 (27) XThreadSetTimeSensitive
    reinterpret_cast<void*>(&atq_thread_is_ts),        // 0xE0 (28) XThreadIsTimeSensitive
    reinterpret_cast<void*>(&atq_assert_not_time_sensitive), // 0xE8 (29) XThreadAssertNotTimeSensitive
    reinterpret_cast<void*>(&atq_unhandled<30>),
    reinterpret_cast<void*>(&atq_unhandled<31>),
};
WsxAsyncTaskQueueComImpl g_atq_singleton{g_atq_vtable, {1}};

// Internal GDK IXError COM table (queried by XCurl.dll / GDK titles for XErrorReport)
struct WsxErrorComImpl {
    void** vtable;
    std::atomic<ULONG> ref{1};
};
HRESULT STDMETHODCALLTYPE err_qi(WsxErrorComImpl* self, REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == kGuidErrorApi) {
        *ppv = self;
        self->ref.fetch_add(1);
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}
ULONG STDMETHODCALLTYPE err_addref(WsxErrorComImpl* self) { return self->ref.fetch_add(1) + 1; }
ULONG STDMETHODCALLTYPE err_release(WsxErrorComImpl* self) { return self->ref.fetch_sub(1) - 1; }
bool STDMETHODCALLTYPE err_report(WsxErrorComImpl*, HRESULT hr, const char* msg) {
    if (GetEnvironmentVariableW(L"WSX_TRACE_API_IMPL", nullptr, 0) > 0) {
        std::fprintf(stderr, "      [xgameruntime::IXError::XErrorReport] hr=0x%08lX msg=%s\n",
                     static_cast<unsigned long>(hr), msg ? msg : "(null)");
    }
    wsx::trace_hr(kMod, "XErrorReport", hr, "msg=%s", msg ? msg : "(null)");
    return true;
}
void* g_err_vtable[8] = {
    reinterpret_cast<void*>(&err_qi),      // 0x00 (0)
    reinterpret_cast<void*>(&err_addref),  // 0x08 (1)
    reinterpret_cast<void*>(&err_release), // 0x10 (2)
    reinterpret_cast<void*>(&err_report),  // 0x18 (3) XErrorReport
    reinterpret_cast<void*>(&err_report),
    reinterpret_cast<void*>(&err_report),
    reinterpret_cast<void*>(&err_report),
    reinterpret_cast<void*>(&err_report),
};
WsxErrorComImpl g_err_singleton{g_err_vtable, {1}};

// Internal GDK IXNetworking COM table (queried by XCurl.dll)
struct WsxNetworkingComImpl {
    void** vtable;
    std::atomic<ULONG> ref{1};
};

HRESULT STDMETHODCALLTYPE net_qi(WsxNetworkingComImpl* self, REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == kGuidNetworkingIid || riid == kGuidNetworkingClsid) {
        *ppv = self;
        self->ref.fetch_add(1);
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}
ULONG STDMETHODCALLTYPE net_addref(WsxNetworkingComImpl* self) { return self->ref.fetch_add(1) + 1; }
ULONG STDMETHODCALLTYPE net_release(WsxNetworkingComImpl* self) { return self->ref.fetch_sub(1) - 1; }

template <uint32_t Slot>
HRESULT STDMETHODCALLTYPE net_unhandled(WsxNetworkingComImpl*) {
    if (GetEnvironmentVariableW(L"WSX_TRACE_API_IMPL", nullptr, 0) > 0) {
        std::fprintf(stderr, "      [xgameruntime::IXNetworking] unhandled slot %u (0x%02X)\n", Slot, Slot * 8);
    }
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE net_query_sec_info_utf16_async(
    WsxNetworkingComImpl*, const wchar_t* url, XAsyncBlock* asyncBlock) {
    return XNetworkingQuerySecurityInformationForUrlUtf16Async(url ? url : L"https://localhost/", asyncBlock);
}
HRESULT STDMETHODCALLTYPE net_query_sec_info_utf16_result_size(
    WsxNetworkingComImpl*, XAsyncBlock* asyncBlock, size_t* resultBufSize) {
    return XNetworkingQuerySecurityInformationForUrlUtf16ResultSize(asyncBlock, resultBufSize);
}
HRESULT STDMETHODCALLTYPE net_query_sec_info_utf16_result(
    WsxNetworkingComImpl*,
    XAsyncBlock* asyncBlock,
    size_t bufSize,
    size_t* bufUsed,
    uint8_t* buf,
    XNetworkingSecurityInformation** outInfo) {
    return XNetworkingQuerySecurityInformationForUrlUtf16Result(
        asyncBlock, bufSize, bufUsed, buf, outInfo);
}
HRESULT STDMETHODCALLTYPE net_verify_server_cert(
    WsxNetworkingComImpl*, void* req, const XNetworkingThumbprint* tp) {
    return XNetworkingVerifyServerCertificate(req, tp);
}
HRESULT STDMETHODCALLTYPE net_reg_conn_hint(
    WsxNetworkingComImpl*, XTaskQueueHandle q, void* ctx, XNetworkingConnectivityHintChangedCallback* cb, XTaskQueueRegistrationToken* tok) {
    return XNetworkingRegisterConnectivityHintChanged(q, ctx, cb, tok);
}
bool STDMETHODCALLTYPE net_unreg_conn_hint(WsxNetworkingComImpl*, XTaskQueueRegistrationToken tok, bool wait) {
    return XNetworkingUnregisterConnectivityHintChanged(tok, wait);
}
HRESULT STDMETHODCALLTYPE net_get_conn_hint(WsxNetworkingComImpl*, XNetworkingConnectivityHint* hint) {
    return XNetworkingGetConnectivityHint(hint);
}

void* g_net_vtable[24] = {
    reinterpret_cast<void*>(&net_qi),                                // 0x00 (0)
    reinterpret_cast<void*>(&net_addref),                            // 0x08 (1)
    reinterpret_cast<void*>(&net_release),                           // 0x10 (2)
    reinterpret_cast<void*>(&net_unhandled<3>),                      // 0x18 (3)
    reinterpret_cast<void*>(&net_unhandled<4>),                      // 0x20 (4)
    reinterpret_cast<void*>(&net_unhandled<5>),                      // 0x28 (5)
    reinterpret_cast<void*>(&net_reg_conn_hint),                     // 0x30 (6)
    reinterpret_cast<void*>(&net_unreg_conn_hint),                   // 0x38 (7)
    reinterpret_cast<void*>(&net_unhandled<8>),                      // 0x40 (8)
    reinterpret_cast<void*>(&net_unhandled<9>),                      // 0x48 (9)
    reinterpret_cast<void*>(&net_unhandled<10>),                     // 0x50 (10)
    reinterpret_cast<void*>(&net_query_sec_info_utf16_async),        // 0x58 (11)
    reinterpret_cast<void*>(&net_query_sec_info_utf16_result_size),  // 0x60 (12)
    reinterpret_cast<void*>(&net_query_sec_info_utf16_result),       // 0x68 (13)
    reinterpret_cast<void*>(&net_verify_server_cert),                // 0x70 (14)
    reinterpret_cast<void*>(&net_get_conn_hint),                     // 0x78 (15)
    reinterpret_cast<void*>(&net_reg_conn_hint),                     // 0x80 (16)
    reinterpret_cast<void*>(&net_unreg_conn_hint),                   // 0x88 (17)
    reinterpret_cast<void*>(&net_unhandled<18>),                     // 0x90 (18)
    reinterpret_cast<void*>(&net_unhandled<19>),                     // 0x98 (19)
    reinterpret_cast<void*>(&net_unhandled<20>),                     // 0xA0 (20)
    reinterpret_cast<void*>(&net_unhandled<21>),                     // 0xA8 (21)
    reinterpret_cast<void*>(&net_unhandled<22>),                     // 0xB0 (22)
    reinterpret_cast<void*>(&net_unhandled<23>),                     // 0xB8 (23)
};
WsxNetworkingComImpl g_net_singleton{g_net_vtable, {1}};

// ----------------------------------------------------------------------------
// Additional GDK QueryApiImpl COM Singletons (ScarlettX / Leonida)
// ----------------------------------------------------------------------------

// XGameImpl: CLSID {973A344E-24BF-4D0F-8457-56C534892B29}, IID {2549F142-6419-4A06-97B5-931AAB7C2F34}
constexpr GUID kGuidXGameClsid = {
    0x973A344Eu, 0x24BFu, 0x4D0Fu, {0x84u, 0x57u, 0x56u, 0xC5u, 0x34u, 0x89u, 0x2Bu, 0x29u}
};
constexpr GUID kGuidXGameIid = {
    0x2549F142u, 0x6419u, 0x4A06u, {0x97u, 0xB5u, 0x93u, 0x1Au, 0xABu, 0x7Cu, 0x2Fu, 0x34u}
};

// XSystemImpl: CLSID {E349BD1A-FC20-4E40-B99C-4178CC6B409F}, IID {67CE4BFC-B1D1-4AC7-BC3A-CB9219A97A85}
constexpr GUID kGuidXSystemClsid = {
    0xE349BD1Au, 0xFC20u, 0x4E40u, {0xB9u, 0x9Cu, 0x41u, 0x78u, 0xCCu, 0x6Bu, 0x40u, 0x9Fu}
};
constexpr GUID kGuidXSystemIid = {
    0x67CE4BFCu, 0xB1D1u, 0x4AC7u, {0xBCu, 0x3Au, 0xCBu, 0x92u, 0x19u, 0xA9u, 0x7Au, 0x85u}
};

// XGameRuntimeFeatureImpl: {8836FE87-EDB9-4FE3-8DAD-05F0D2CD5B40}
constexpr GUID kGuidXGameRuntimeFeature = {
    0x8836FE87u, 0xEDB9u, 0x4FE3u, {0x8Du, 0xADu, 0x05u, 0xF0u, 0xD2u, 0xCDu, 0x5Bu, 0x40u}
};

// XGameEventImpl: {BBFBDCC7-BFE7-409B-A5CA-EDF054960B4D}
constexpr GUID kGuidXGameEvent = {
    0xBBFBDCC7u, 0xBFE7u, 0x409Bu, {0xA5u, 0xCAu, 0xEDu, 0xF0u, 0x54u, 0x96u, 0x0Bu, 0x4Du}
};

// XUserGamertagExtImpl: {CEF4FAC0-7676-4A94-A119-4C43F9EB5B74}
// XUserImpl: CLSID {01ACD177-91F9-4763-A38E-CCBB55CE32E0}, IID {079415E3-6727-437F-8E9D-8F8F9B2439F7}
constexpr GUID kGuidXUserGamertagExt = {
    0xCEF4FAC0u, 0x7676u, 0x4A94u, {0xA1u, 0x19u, 0x4Cu, 0x43u, 0xF9u, 0xEBu, 0x5Bu, 0x74u}
};
constexpr GUID kGuidXUserClsid = {
    0x01ACD177u, 0x91F9u, 0x4763u, {0xA3u, 0x8Eu, 0xCCu, 0xBBu, 0x55u, 0xCEu, 0x32u, 0xE0u}
};
constexpr GUID kGuidXUserIid = {
    0x079415E3u, 0x6727u, 0x437Fu, {0x8Eu, 0x9Du, 0x8Fu, 0x8Fu, 0x9Bu, 0x24u, 0x39u, 0xF7u}
};

// XPackageImpl: CLSID {AF406016-E850-4AA8-A88D-2F3DCB9DAC7E}, IID {F92D8712-2B27-4D8A-BF01-11A6F8E3EB42}
constexpr GUID kGuidXPackageClsid = {
    0xAF406016u, 0xE850u, 0x4AA8u, {0xA8u, 0x8Du, 0x2Fu, 0x3Du, 0xCBu, 0x9Du, 0xACu, 0x7Eu}
};
constexpr GUID kGuidXPackageIid = {
    0xF92D8712u, 0x2B27u, 0x4D8Au, {0xBFu, 0x01u, 0x11u, 0xA6u, 0xF8u, 0xE3u, 0xEBu, 0x42u}
};

struct WsxGenericComImpl {
    void** vtable;
    std::atomic<ULONG> ref{1};
};

ULONG STDMETHODCALLTYPE com_generic_addref(WsxGenericComImpl* self) {
    return self->ref.fetch_add(1) + 1;
}
ULONG STDMETHODCALLTYPE com_generic_release(WsxGenericComImpl* self) {
    return self->ref.fetch_sub(1) - 1;
}
HRESULT STDMETHODCALLTYPE com_generic_noop_ok(void* /*self*/) {
    return S_OK;
}

// 1) XGameImpl
HRESULT STDMETHODCALLTYPE xgame_qi(WsxGenericComImpl* self, REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == kGuidXGameClsid || riid == kGuidXGameIid) {
        *ppv = self;
        self->ref.fetch_add(1);
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}
HRESULT STDMETHODCALLTYPE xgame_get_xbox_title_id(void* /*self*/, uint32_t* outTitleId) {
    if (!outTitleId) return E_INVALIDARG;
    const ParsedGameIdentity id = ReadLocalGameConfigIdentity();
    unsigned long parsed = 0ul;
    if (!id.titleId.empty()) {
        parsed = std::strtoul(id.titleId.c_str(), nullptr, 16);
    }
    *outTitleId = (parsed != 0ul) ? static_cast<uint32_t>(parsed) : 0x00007777u;
    return wsx::trace_hr(kMod, "XGameGetXboxTitleId", S_OK, "titleId=0x%08X", *outTitleId);
}
void* g_xgame_vtable[8] = {
    reinterpret_cast<void*>(&xgame_qi),                // 0x00 (0)
    reinterpret_cast<void*>(&com_generic_addref),      // 0x08 (1)
    reinterpret_cast<void*>(&com_generic_release),     // 0x10 (2)
    reinterpret_cast<void*>(&xgame_get_xbox_title_id), // 0x18 (3)
    reinterpret_cast<void*>(&com_generic_noop_ok),     // 0x20 (4)
    reinterpret_cast<void*>(&com_generic_noop_ok),     // 0x28 (5)
    reinterpret_cast<void*>(&com_generic_noop_ok),     // 0x30 (6)
    reinterpret_cast<void*>(&com_generic_noop_ok),     // 0x38 (7)
};
WsxGenericComImpl g_xgame_singleton{g_xgame_vtable, {1}};

// 2) XSystemImpl
HRESULT STDMETHODCALLTYPE xsys_qi(WsxGenericComImpl* self, REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == kGuidXSystemClsid || riid == kGuidXSystemIid) {
        *ppv = self;
        self->ref.fetch_add(1);
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}
HRESULT STDMETHODCALLTYPE xsys_get_console_id(void* /*self*/, size_t size, char* buf, size_t* used) {
    return XSystemGetConsoleId(size, buf, used);
}
HRESULT STDMETHODCALLTYPE xsys_get_sandbox_id(void* /*self*/, size_t size, char* buf, size_t* used) {
    return XSystemGetXboxLiveSandboxId(size, buf, used);
}
HRESULT STDMETHODCALLTYPE xsys_get_app_device_id(void* /*self*/, size_t size, char* buf, size_t* used) {
    return XSystemGetAppSpecificDeviceId(size, buf, used);
}
XSystemDeviceType STDMETHODCALLTYPE xsys_get_device_type(void* /*self*/) {
    return XSystemGetDeviceType();
}
void* g_xsystem_vtable[12] = {
    reinterpret_cast<void*>(&xsys_qi),               // 0x00 (0)
    reinterpret_cast<void*>(&com_generic_addref),    // 0x08 (1)
    reinterpret_cast<void*>(&com_generic_release),   // 0x10 (2)
    reinterpret_cast<void*>(&xsys_get_console_id),   // 0x18 (3)
    reinterpret_cast<void*>(&xsys_get_sandbox_id),   // 0x20 (4)
    reinterpret_cast<void*>(&xsys_get_app_device_id),// 0x28 (5)
    reinterpret_cast<void*>(&xsys_get_device_type),  // 0x30 (6)
    reinterpret_cast<void*>(&com_generic_noop_ok),   // 0x38 (7)
    reinterpret_cast<void*>(&com_generic_noop_ok),   // 0x40 (8)
    reinterpret_cast<void*>(&com_generic_noop_ok),   // 0x48 (9)
    reinterpret_cast<void*>(&com_generic_noop_ok),   // 0x50 (10)
    reinterpret_cast<void*>(&com_generic_noop_ok),   // 0x58 (11)
};
WsxGenericComImpl g_xsystem_singleton{g_xsystem_vtable, {1}};

// 3) XGameRuntimeFeatureImpl
HRESULT STDMETHODCALLTYPE xgr_feat_qi(WsxGenericComImpl* self, REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == kGuidXGameRuntimeFeature) {
        *ppv = self;
        self->ref.fetch_add(1);
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}
bool STDMETHODCALLTYPE xgr_is_feature_avail(void* /*self*/, uint32_t feature) {
    return feature <= 21u;
}
void* g_xgr_feature_vtable[8] = {
    reinterpret_cast<void*>(&xgr_feat_qi),          // 0x00 (0)
    reinterpret_cast<void*>(&com_generic_addref),   // 0x08 (1)
    reinterpret_cast<void*>(&com_generic_release),  // 0x10 (2)
    reinterpret_cast<void*>(&xgr_is_feature_avail), // 0x18 (3)
    reinterpret_cast<void*>(&xgr_is_feature_avail),
    reinterpret_cast<void*>(&xgr_is_feature_avail),
    reinterpret_cast<void*>(&xgr_is_feature_avail),
    reinterpret_cast<void*>(&xgr_is_feature_avail),
};
WsxGenericComImpl g_xgr_feature_singleton{g_xgr_feature_vtable, {1}};

// 4) XGameEventImpl
HRESULT STDMETHODCALLTYPE xgame_event_qi(WsxGenericComImpl* self, REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == kGuidXGameEvent) {
        *ppv = self;
        self->ref.fetch_add(1);
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}
HRESULT STDMETHODCALLTYPE xgame_event_write(void* /*self*/, ...) {
    return S_OK;
}
void* g_xgame_event_vtable[8] = {
    reinterpret_cast<void*>(&xgame_event_qi),       // 0x00 (0)
    reinterpret_cast<void*>(&com_generic_addref),   // 0x08 (1)
    reinterpret_cast<void*>(&com_generic_release),  // 0x10 (2)
    reinterpret_cast<void*>(&xgame_event_write),    // 0x18 (3)
    reinterpret_cast<void*>(&xgame_event_write),
    reinterpret_cast<void*>(&xgame_event_write),
    reinterpret_cast<void*>(&xgame_event_write),
    reinterpret_cast<void*>(&xgame_event_write),
};
WsxGenericComImpl g_xgame_event_singleton{g_xgame_event_vtable, {1}};

// 5) XUserGamertagExtImpl / XUserImpl
HRESULT STDMETHODCALLTYPE xuser_ext_qi(WsxGenericComImpl* self, REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) ||
        riid == kGuidXUserGamertagExt ||
        riid == kGuidXUserClsid ||
        riid == kGuidXUserIid) {
        *ppv = self;
        self->ref.fetch_add(1);
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}
HRESULT STDMETHODCALLTYPE xuser_get_gamertag_com(
    void* /*self*/,
    XUserHandle user,
    XUserGamertagComponent component,
    size_t size,
    char* buf,
    size_t* used) {
    return XUserGetGamertag(user, component, size, buf, used);
}
void* g_xuser_ext_vtable[24] = {
    reinterpret_cast<void*>(&xuser_ext_qi),           // 0x00 (0)
    reinterpret_cast<void*>(&com_generic_addref),     // 0x08 (1)
    reinterpret_cast<void*>(&com_generic_release),    // 0x10 (2)
    reinterpret_cast<void*>(&xuser_get_gamertag_com), // 0x18 (3)
    reinterpret_cast<void*>(&com_generic_noop_ok),
    reinterpret_cast<void*>(&com_generic_noop_ok),
    reinterpret_cast<void*>(&com_generic_noop_ok),
    reinterpret_cast<void*>(&com_generic_noop_ok),
    reinterpret_cast<void*>(&com_generic_noop_ok),
    reinterpret_cast<void*>(&com_generic_noop_ok),
    reinterpret_cast<void*>(&com_generic_noop_ok),
    reinterpret_cast<void*>(&com_generic_noop_ok),
    reinterpret_cast<void*>(&com_generic_noop_ok),
    reinterpret_cast<void*>(&com_generic_noop_ok),
    reinterpret_cast<void*>(&com_generic_noop_ok),
    reinterpret_cast<void*>(&com_generic_noop_ok),
    reinterpret_cast<void*>(&com_generic_noop_ok),
    reinterpret_cast<void*>(&com_generic_noop_ok),
    reinterpret_cast<void*>(&com_generic_noop_ok),
    reinterpret_cast<void*>(&com_generic_noop_ok),
    reinterpret_cast<void*>(&com_generic_noop_ok),
    reinterpret_cast<void*>(&com_generic_noop_ok),
    reinterpret_cast<void*>(&com_generic_noop_ok),
    reinterpret_cast<void*>(&com_generic_noop_ok),
};
WsxGenericComImpl g_xuser_ext_singleton{g_xuser_ext_vtable, {1}};

// 6) XPackageImpl (39-slot table)
HRESULT STDMETHODCALLTYPE xpkg_qi(WsxGenericComImpl* self, REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == kGuidXPackageClsid || riid == kGuidXPackageIid) {
        *ppv = self;
        self->ref.fetch_add(1);
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}
HRESULT STDMETHODCALLTYPE xpkg_get_cur_proc_id(void* /*self*/, size_t bufferSize, char* buffer) {
    return XPackageGetCurrentProcessPackageIdentifier(bufferSize, buffer);
}
bool STDMETHODCALLTYPE xpkg_is_packaged_proc(void* /*self*/) {
    return XPackageIsPackagedProcess();
}
HRESULT STDMETHODCALLTYPE xpkg_get_user_locale(void* /*self*/, size_t localeSize, char* locale) {
    return XPackageGetUserLocale(localeSize, locale);
}
HRESULT STDMETHODCALLTYPE xpkg_enum_packages(
    void* /*self*/,
    XPackageKind kind,
    XPackageEnumerationScope scope,
    void* context,
    XPackageEnumerationCallback* callback) {
    return XPackageEnumeratePackages(kind, scope, context, callback);
}
void* g_xpackage_vtable[39] = {
    reinterpret_cast<void*>(&xpkg_qi),               // 0x000 (0)
    reinterpret_cast<void*>(&com_generic_addref),    // 0x008 (1)
    reinterpret_cast<void*>(&com_generic_release),   // 0x010 (2)
    reinterpret_cast<void*>(&xpkg_get_cur_proc_id),  // 0x018 (3) XPackageGetCurrentProcessPackageIdentifier
    reinterpret_cast<void*>(&xpkg_is_packaged_proc), // 0x020 (4) XPackageIsPackagedProcess
    reinterpret_cast<void*>(&com_generic_noop_ok),   // 0x028 (5)
    reinterpret_cast<void*>(&com_generic_noop_ok),   // 0x030 (6)
    reinterpret_cast<void*>(&com_generic_noop_ok),   // 0x038 (7)
    reinterpret_cast<void*>(&com_generic_noop_ok),   // 0x040 (8)
    reinterpret_cast<void*>(&com_generic_noop_ok),   // 0x048 (9)
    reinterpret_cast<void*>(&com_generic_noop_ok),   // 0x050 (10)
    reinterpret_cast<void*>(&xpkg_get_user_locale),  // 0x058 (11) XPackageGetUserLocale
    reinterpret_cast<void*>(&com_generic_noop_ok),   // 0x060 (12)
    reinterpret_cast<void*>(&com_generic_noop_ok),   // 0x068 (13)
    reinterpret_cast<void*>(&com_generic_noop_ok),   // 0x070 (14)
    reinterpret_cast<void*>(&com_generic_noop_ok),   // 0x078 (15)
    reinterpret_cast<void*>(&com_generic_noop_ok),   // 0x080 (16)
    reinterpret_cast<void*>(&com_generic_noop_ok),   // 0x088 (17)
    reinterpret_cast<void*>(&com_generic_noop_ok),   // 0x090 (18)
    reinterpret_cast<void*>(&com_generic_noop_ok),   // 0x098 (19)
    reinterpret_cast<void*>(&xpkg_enum_packages),    // 0x0A0 (20) XPackageEnumeratePackages
    reinterpret_cast<void*>(&com_generic_noop_ok),   // 0x0A8 (21)
    reinterpret_cast<void*>(&com_generic_noop_ok),   // 0x0B0 (22)
    reinterpret_cast<void*>(&com_generic_noop_ok),   // 0x0B8 (23)
    reinterpret_cast<void*>(&com_generic_noop_ok),   // 0x0C0 (24)
    reinterpret_cast<void*>(&com_generic_noop_ok),   // 0x0C8 (25)
    reinterpret_cast<void*>(&com_generic_noop_ok),   // 0x0D0 (26)
    reinterpret_cast<void*>(&com_generic_noop_ok),   // 0x0D8 (27)
    reinterpret_cast<void*>(&xpkg_enum_packages),    // 0x0E0 (28) XPackageEnumeratePackages
    reinterpret_cast<void*>(&com_generic_noop_ok),   // 0x0E8 (29)
    reinterpret_cast<void*>(&com_generic_noop_ok),   // 0x0F0 (30)
    reinterpret_cast<void*>(&com_generic_noop_ok),   // 0x0F8 (31)
    reinterpret_cast<void*>(&com_generic_noop_ok),   // 0x100 (32)
    reinterpret_cast<void*>(&com_generic_noop_ok),   // 0x108 (33)
    reinterpret_cast<void*>(&com_generic_noop_ok),   // 0x110 (34)
    reinterpret_cast<void*>(&xpkg_enum_packages),    // 0x118 (35) XPackageEnumeratePackages
    reinterpret_cast<void*>(&com_generic_noop_ok),   // 0x120 (36)
    reinterpret_cast<void*>(&com_generic_noop_ok),   // 0x128 (37)
    reinterpret_cast<void*>(&com_generic_noop_ok),   // 0x130 (38)
};
WsxGenericComImpl g_xpackage_singleton{g_xpackage_vtable, {1}};

// 7) Generic 96-slot Discovery COM Singleton (when WSX_QUERY_API_DISCOVERY is enabled)
HRESULT STDMETHODCALLTYPE disc_qi(WsxGenericComImpl* self, REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    wchar_t g[64]{};
    StringFromGUID2(riid, g, 64);
    std::fprintf(stderr, "      [xgameruntime::DiscoveryCOM] QueryInterface riid=%ls -> S_OK\n", g);
    wsx::trace_hr(kMod, "DiscoveryCOM::QueryInterface", S_OK, "riid=%ls", g);
    *ppv = self;
    self->ref.fetch_add(1);
    return S_OK;
}
template <size_t Slot>
HRESULT STDMETHODCALLTYPE disc_slot(WsxGenericComImpl* /*self*/) {
    std::fprintf(stderr, "      [xgameruntime::DiscoveryCOM] invoked slot %zu (+0x%02zX) -> E_NOTIMPL\n",
                 Slot, Slot * sizeof(void*));
    return wsx::trace_hr(kMod, "DiscoveryCOM::Slot", E_NOTIMPL, "slot=%zu offset=0x%02zX",
                         Slot, Slot * sizeof(void*));
}
template <size_t... Is>
void InitDiscoveryVtable(void* (&vt)[96], std::index_sequence<Is...>) {
    vt[0] = reinterpret_cast<void*>(&disc_qi);
    vt[1] = reinterpret_cast<void*>(&com_generic_addref);
    vt[2] = reinterpret_cast<void*>(&com_generic_release);
    ((vt[Is + 3] = reinterpret_cast<void*>(&disc_slot<Is + 3>)), ...);
}
WsxGenericComImpl* GetDiscoverySingleton() noexcept {
    static void* s_disc_vtable[96]{};
    static WsxGenericComImpl s_disc_singleton{s_disc_vtable, {1}};
    static std::once_flag s_once;
    std::call_once(s_once, []() {
        InitDiscoveryVtable(s_disc_vtable, std::make_index_sequence<93>{});
    });
    return &s_disc_singleton;
}

} // namespace

extern "C" {

HRESULT STDAPICALLTYPE InitializeApiImpl(uint32_t major, uint32_t minor) noexcept {
    const HRESULT hr = XGameRuntimeInitialize();
    return wsx::trace_hr(kMod, "InitializeApiImpl", hr, "v=%u.%u", major, minor);
}

HRESULT STDAPICALLTYPE InitializeApiImplEx(uint64_t ver1, uint64_t ver2, uint64_t flags) noexcept {
    const HRESULT hr = XGameRuntimeInitialize();
    return wsx::trace_hr(kMod, "InitializeApiImplEx", hr, "v1=0x%llX v2=0x%llX flags=0x%llX",
                         static_cast<unsigned long long>(ver1),
                         static_cast<unsigned long long>(ver2),
                         static_cast<unsigned long long>(flags));
}

HRESULT STDAPICALLTYPE InitializeApiImplEx2(uint64_t ver1, uint64_t ver2, uint64_t flags, const void* /*options*/) noexcept {
    const HRESULT hr = XGameRuntimeInitialize();
    return wsx::trace_hr(kMod, "InitializeApiImplEx2", hr, "v1=0x%llX v2=0x%llX flags=0x%llX",
                         static_cast<unsigned long long>(ver1),
                         static_cast<unsigned long long>(ver2),
                         static_cast<unsigned long long>(flags));
}

void STDAPICALLTYPE UninitializeApiImpl() noexcept {
    XGameRuntimeUninitialize();
}

bool STDAPICALLTYPE XErrorReport(HRESULT hr, const char* msg) noexcept {
    wsx::trace_hr(kMod, "XErrorReport", hr, "msg=%s", msg ? msg : "(null)");
    return true;
}

HRESULT STDAPICALLTYPE QueryApiImpl(const GUID* clsid, const GUID* iid, void** ppv) noexcept {
    if (ppv) *ppv = nullptr;
    if (!clsid || !iid || !ppv) return E_INVALIDARG;
    if (*clsid == kGuidAsyncTaskQueueApi || *iid == kGuidAsyncTaskQueueApi) {
        g_atq_singleton.ref.fetch_add(1);
        *ppv = &g_atq_singleton;
        return S_OK;
    }
    if (*clsid == kGuidNetworkingClsid || *clsid == kGuidNetworkingIid ||
        *iid == kGuidNetworkingIid || *iid == kGuidNetworkingClsid) {
        g_net_singleton.ref.fetch_add(1);
        *ppv = &g_net_singleton;
        return S_OK;
    }
    if (*clsid == kGuidErrorApi || *iid == kGuidErrorApi) {
        g_err_singleton.ref.fetch_add(1);
        *ppv = &g_err_singleton;
        return S_OK;
    }
    if (*clsid == kGuidXGameClsid || *clsid == kGuidXGameIid ||
        *iid == kGuidXGameClsid || *iid == kGuidXGameIid) {
        g_xgame_singleton.ref.fetch_add(1);
        *ppv = &g_xgame_singleton;
        return S_OK;
    }
    if (*clsid == kGuidXSystemClsid || *clsid == kGuidXSystemIid ||
        *iid == kGuidXSystemClsid || *iid == kGuidXSystemIid) {
        g_xsystem_singleton.ref.fetch_add(1);
        *ppv = &g_xsystem_singleton;
        return S_OK;
    }
    if (*clsid == kGuidXGameRuntimeFeature || *iid == kGuidXGameRuntimeFeature) {
        g_xgr_feature_singleton.ref.fetch_add(1);
        *ppv = &g_xgr_feature_singleton;
        return S_OK;
    }
    if (*clsid == kGuidXGameEvent || *iid == kGuidXGameEvent) {
        g_xgame_event_singleton.ref.fetch_add(1);
        *ppv = &g_xgame_event_singleton;
        return S_OK;
    }
    if (*clsid == kGuidXUserGamertagExt || *clsid == kGuidXUserClsid || *clsid == kGuidXUserIid ||
        *iid == kGuidXUserGamertagExt || *iid == kGuidXUserClsid || *iid == kGuidXUserIid) {
        g_xuser_ext_singleton.ref.fetch_add(1);
        *ppv = &g_xuser_ext_singleton;
        return S_OK;
    }
    if (*clsid == kGuidXPackageClsid || *clsid == kGuidXPackageIid ||
        *iid == kGuidXPackageClsid || *iid == kGuidXPackageIid) {
        g_xpackage_singleton.ref.fetch_add(1);
        *ppv = &g_xpackage_singleton;
        return S_OK;
    }
    wchar_t g1[64]{}, g2[64]{};
    StringFromGUID2(*clsid, g1, 64);
    StringFromGUID2(*iid, g2, 64);
    if (wsx::get_config_bool("WSX_QUERY_API_DISCOVERY", "Console", "query_api_discovery", false)) {
        std::fprintf(stderr, "      [xgameruntime::QueryApiImpl] DISCOVERY clsid=%ls iid=%ls -> S_OK (96-slot discovery COM)\n", g1, g2);
        auto* disc = GetDiscoverySingleton();
        disc->ref.fetch_add(1);
        *ppv = disc;
        return wsx::trace_hr(kMod, "QueryApiImpl", S_OK, "discovery=1 clsid=%ls iid=%ls", g1, g2);
    }
    if (GetEnvironmentVariableW(L"WSX_TRACE_API_IMPL", nullptr, 0) > 0) {
        std::fprintf(stderr, "      [xgameruntime::QueryApiImpl] clsid=%ls iid=%ls -> REGDB_E_CLASSNOTREG\n", g1, g2);
    }
    return wsx::trace_hr(kMod, "QueryApiImpl", REGDB_E_CLASSNOTREG, "clsid=%ls iid=%ls", g1, g2);
}

// ============================================================================
// XGameStreaming Offline Exports (Cloud Gaming / Remote Play clean-room stubs)
// ============================================================================
namespace {
std::atomic<bool> g_game_streaming_initialized{false};
std::mutex g_game_streaming_mtx;
uint64_t g_game_streaming_next_token = 1u;
std::vector<uint64_t> g_game_streaming_tokens;
} // namespace

HRESULT STDAPICALLTYPE XGameStreamingInitialize() noexcept {
    g_game_streaming_initialized.store(true, std::memory_order_release);
    return wsx::trace_hr(kMod, "XGameStreamingInitialize", S_OK);
}

void STDAPICALLTYPE XGameStreamingUninitialize() noexcept {
    g_game_streaming_initialized.store(false, std::memory_order_release);
    std::lock_guard<std::mutex> lk(g_game_streaming_mtx);
    g_game_streaming_tokens.clear();
    wsx::trace_void(kMod, "XGameStreamingUninitialize", "ok");
}

bool STDAPICALLTYPE XGameStreamingIsStreaming() noexcept {
    return false;
}

uint32_t STDAPICALLTYPE XGameStreamingGetClientCount() noexcept {
    return 0u;
}

HRESULT STDAPICALLTYPE XGameStreamingGetClients(
    uint32_t /*clientCount*/,
    XGameStreamingClientId* /*clients*/,
    uint32_t* clientsUsed) noexcept {
    if (clientsUsed) *clientsUsed = 0u;
    if (!g_game_streaming_initialized.load(std::memory_order_acquire)) {
        return E_GAMESTREAMING_NOT_INITIALIZED;
    }
    return S_OK;
}

void STDAPICALLTYPE XGameStreamingHideTouchControls() noexcept {}

void STDAPICALLTYPE XGameStreamingShowTouchControlLayout(const char* /*layout*/) noexcept {}

HRESULT STDAPICALLTYPE XGameStreamingGetTouchBundleVersion(
    XGameStreamingClientId /*client*/,
    XVersion* version,
    size_t /*versionNameSize*/,
    char* /*versionName*/) noexcept {
    if (version) *version = XVersion{};
    if (!g_game_streaming_initialized.load(std::memory_order_acquire)) {
        return E_GAMESTREAMING_NOT_INITIALIZED;
    }
    return E_GAMESTREAMING_CLIENT_NOT_CONNECTED;
}

HRESULT STDAPICALLTYPE XGameStreamingRegisterConnectionStateChanged(
    XTaskQueueHandle /*queue*/,
    void* /*context*/,
    XGameStreamingConnectionStateChangedCallback* callback,
    XTaskQueueRegistrationToken* token) noexcept {
    if (!callback || !token) return E_INVALIDARG;
    if (!g_game_streaming_initialized.load(std::memory_order_acquire)) {
        token->token = 0u;
        return E_GAMESTREAMING_NOT_INITIALIZED;
    }
    std::lock_guard<std::mutex> lk(g_game_streaming_mtx);
    const uint64_t tok = g_game_streaming_next_token++;
    g_game_streaming_tokens.push_back(tok);
    token->token = tok;
    return S_OK;
}

bool STDAPICALLTYPE XGameStreamingUnregisterConnectionStateChanged(
    XTaskQueueRegistrationToken token,
    bool /*wait*/) noexcept {
    std::lock_guard<std::mutex> lk(g_game_streaming_mtx);
    const auto it = std::find(g_game_streaming_tokens.begin(), g_game_streaming_tokens.end(), token.token);
    if (it == g_game_streaming_tokens.end()) return false;
    g_game_streaming_tokens.erase(it);
    return true;
}

} // extern "C"

#include "xgameruntime_offline_xsapi.inl"

#pragma once
#include "NativeModules.h"
#include "winrt/Microsoft.ReactNative.h"
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Data.Json.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <fstream>
#include <sstream>

using namespace winrt::Microsoft::ReactNative;

// ============================================================================
// SyncKvModule - synchronous, file-backed key/value store for react-native-windows.
//
// WHY: react-native-windows ships NO synchronous, JS-accessible persistent
// storage. AsyncStorage is async by definition; react-native-mmkv v4 is
// Nitro/native with no Windows (UWP/WinAppSDK) build; expo-secure-store and
// react-native-encrypted-storage have no Windows implementation either. Any
// offline-first app that wants data available INSTANTLY at module-load time
// (the MMKV programming model: `storage.getString(k)` returning a value, not a
// promise) has nowhere to go on Windows. This module provides that primitive.
//
// It hydrates a single JSON blob from ApplicationData.LocalFolder into an
// in-memory map ONCE (mirroring MMKV's mmap-hydrate-once model) using
// synchronous std::ifstream, then serves REACT_SYNC_METHOD reads/writes with
// zero async hops. Writes are debounced and flushed on a background thread via
// an atomic temp-file replace.
//
// LIFETIME (memory-safety critical — this fixed a real heap-corruption crash):
// React module instances are created/destroyed PER REACT INSTANCE — every Metro
// reload tears the module down. The original design kept the map/mutex/cv as
// instance members and spawned a detached worker thread capturing `this`; after
// a reload the orphaned worker kept reading and writing the FREED instance —
// classic use-after-free that presented as random Hermes access violations /
// fail-fasts (three distinct crash signatures, worse with bigger data). ALL
// mutable state now lives in a process-lifetime singleton that is deliberately
// LEAKED (never destroyed, so the worker can never outlive it — the OS reclaims
// everything at process death), and exactly ONE worker thread exists per
// process regardless of how many times the React instance reloads.
//
// Registered by ReactPackageProvider::CreatePackage via
// AddAttributedModules(builder, true); exposed to JS as
// NativeModules.RNWSyncKv.
// ============================================================================

namespace winrt::RNWSyncKv::implementation
{
    namespace SyncKvDetail
    {
        struct State
        {
            std::unordered_map<std::string, std::string> map;
            std::mutex mapMutex;
            bool loaded{ false };

            // Debounced-flush machinery. `dirty` and `lastWrite` are only ever
            // read/written under cvMutex so a ScheduleFlush can never slip a
            // notify between the worker's predicate check and its wait
            // (lost-wakeup race in the previous version).
            bool dirty{ false };
            std::chrono::steady_clock::time_point lastWrite{};
            std::once_flag workerOnce;
            std::mutex cvMutex;
            std::condition_variable cv;

            // Serializes Flush() bodies: clearAll()'s synchronous flush on the
            // JS thread must not race the worker's flush over the same .tmp file.
            std::mutex flushMutex;
        };

        // Deliberately leaked: the worker thread may touch this at any point up
        // to process death, including during static destruction of everything
        // else — a leaked singleton can never dangle.
        inline State& GetState()
        {
            static State* s = new State();
            return *s;
        }

        inline std::wstring FilePath()
        {
            auto folder = winrt::Windows::Storage::ApplicationData::Current().LocalFolder();
            return std::wstring(folder.Path().c_str()) + L"\\RNWSyncKv.json";
        }

        // Hydrate the in-memory map from disk exactly once (caller holds mapMutex).
        // Set loaded first so a parse failure can't cause a retry loop; a corrupt
        // file just starts empty.
        inline void EnsureLoadedLocked(State& s)
        {
            if (s.loaded) return;
            s.loaded = true;
            try
            {
                std::ifstream in(FilePath().c_str(), std::ios::binary);
                if (!in.good()) return;
                std::stringstream ss;
                ss << in.rdbuf();
                const std::string contents = ss.str();
                if (contents.empty()) return;

                auto json = winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(contents));
                for (auto const& pair : json)
                {
                    s.map[winrt::to_string(pair.Key())] = winrt::to_string(pair.Value().GetString());
                }
            }
            catch (...)
            {
                OutputDebugStringA("RNWSyncKv: hydrate failed; starting empty\n");
            }
        }

        // Serialize the whole map (snapshot under the data mutex, IO outside it) and
        // atomically replace RNWSyncKv.json via a sibling temp file. A kill mid-write can
        // only leave the .tmp partial — the real file is always whole. flushMutex
        // serializes concurrent callers (worker vs clearAll) so they can't interleave
        // writes to the same .tmp path.
        inline void Flush() noexcept
        {
            State& s = GetState();
            try
            {
                std::scoped_lock flushLock(s.flushMutex);
                std::string out;
                {
                    std::scoped_lock lock(s.mapMutex);
                    winrt::Windows::Data::Json::JsonObject json;
                    for (auto const& kv : s.map)
                    {
                        json.SetNamedValue(
                            winrt::to_hstring(kv.first),
                            winrt::Windows::Data::Json::JsonValue::CreateStringValue(winrt::to_hstring(kv.second)));
                    }
                    out = winrt::to_string(json.Stringify());
                }

                const std::wstring target = FilePath();
                const std::wstring tmp = target + L".tmp";
                {
                    std::ofstream os(tmp.c_str(), std::ios::binary | std::ios::trunc);
                    os << out;
                    os.flush();
                }
                if (!::MoveFileExW(tmp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING))
                {
                    OutputDebugStringA("RNWSyncKv: atomic replace failed\n");
                }
            }
            catch (...)
            {
                OutputDebugStringA("RNWSyncKv: persist failed\n");
            }
        }

        // Background debounce loop. References ONLY the leaked singleton — never a
        // module instance — so React-instance reloads can't dangle it. Exactly one
        // per process (workerOnce lives in the singleton).
        inline void WorkerLoop() noexcept
        {
            constexpr auto kDebounce = std::chrono::milliseconds(300);
            State& s = GetState();
            std::unique_lock<std::mutex> cvLock(s.cvMutex);
            for (;;)
            {
                s.cv.wait(cvLock, [&s] { return s.dirty; });
                // Debounce: wait until the store has been quiet for kDebounce.
                for (;;)
                {
                    const auto now = std::chrono::steady_clock::now();
                    if (now - s.lastWrite >= kDebounce) break;
                    s.cv.wait_for(cvLock, kDebounce - (now - s.lastWrite));
                }
                s.dirty = false;
                cvLock.unlock();
                Flush();
                cvLock.lock();
                // If a mutation raced in during Flush, dirty is true again and we loop.
            }
        }

        inline void ScheduleFlush() noexcept
        {
            State& s = GetState();
            try
            {
                {
                    std::scoped_lock cvLock(s.cvMutex);
                    s.dirty = true;
                    s.lastWrite = std::chrono::steady_clock::now();
                }
                std::call_once(s.workerOnce, [] {
                    std::thread([] { WorkerLoop(); }).detach();
                });
                s.cv.notify_one();
            }
            catch (...)
            {
                // A failed schedule only delays persistence; never propagate out of
                // a noexcept path. The next mutation reschedules.
                OutputDebugStringA("RNWSyncKv: ScheduleFlush failed\n");
            }
        }
    } // namespace SyncKvDetail

    REACT_MODULE(SyncKvModule, L"RNWSyncKv");
    struct SyncKvModule
    {
        // All methods are noexcept per the RNW sync-method contract, and every body
        // that allocates (multi-MB string copies, map inserts) is guarded — a
        // bad_alloc in a noexcept function would otherwise fast-fail the process.

        REACT_SYNC_METHOD(getString);
        std::string getString(std::string key) noexcept
        {
            try
            {
                auto& s = SyncKvDetail::GetState();
                std::scoped_lock lock(s.mapMutex);
                SyncKvDetail::EnsureLoadedLocked(s);
                auto it = s.map.find(key);
                return it == s.map.end() ? std::string() : it->second;
            }
            catch (...)
            {
                return std::string();
            }
        }

        REACT_SYNC_METHOD(contains);
        bool contains(std::string key) noexcept
        {
            try
            {
                auto& s = SyncKvDetail::GetState();
                std::scoped_lock lock(s.mapMutex);
                SyncKvDetail::EnsureLoadedLocked(s);
                return s.map.find(key) != s.map.end();
            }
            catch (...)
            {
                return false;
            }
        }

        // NOTE: RNW sync methods cannot return void (ModuleSyncMethodInfo requires a
        // TResult), so writes return bool. Kept SYNCHRONOUS (not REACT_METHOD/async) so
        // that a set() immediately followed by a sync getString() sees the new value.
        REACT_SYNC_METHOD(set);
        bool set(std::string key, std::string value) noexcept
        {
            try
            {
                auto& s = SyncKvDetail::GetState();
                {
                    std::scoped_lock lock(s.mapMutex);
                    SyncKvDetail::EnsureLoadedLocked(s);
                    s.map[key] = std::move(value);
                }
                SyncKvDetail::ScheduleFlush();
                return true;
            }
            catch (...)
            {
                return false;
            }
        }

        REACT_SYNC_METHOD(remove);
        bool remove(std::string key) noexcept
        {
            try
            {
                auto& s = SyncKvDetail::GetState();
                bool existed = false;
                {
                    std::scoped_lock lock(s.mapMutex);
                    SyncKvDetail::EnsureLoadedLocked(s);
                    existed = s.map.erase(key) > 0;
                }
                if (existed) SyncKvDetail::ScheduleFlush();
                return existed;
            }
            catch (...)
            {
                return false;
            }
        }

        REACT_SYNC_METHOD(getAllKeys);
        std::vector<std::string> getAllKeys() noexcept
        {
            try
            {
                auto& s = SyncKvDetail::GetState();
                std::scoped_lock lock(s.mapMutex);
                SyncKvDetail::EnsureLoadedLocked(s);
                std::vector<std::string> keys;
                keys.reserve(s.map.size());
                for (auto const& kv : s.map) keys.push_back(kv.first);
                return keys;
            }
            catch (...)
            {
                return {};
            }
        }

        REACT_SYNC_METHOD(clearAll);
        bool clearAll() noexcept
        {
            try
            {
                auto& s = SyncKvDetail::GetState();
                {
                    std::scoped_lock lock(s.mapMutex);
                    SyncKvDetail::EnsureLoadedLocked(s);
                    s.map.clear();
                }
                // Logout/purge correctness: flush synchronously so the wipe is durable NOW
                // (a crash right after logout must not resurrect the previous user's data).
                // Flush serializes against the worker via flushMutex.
                SyncKvDetail::Flush();
                return true;
            }
            catch (...)
            {
                return false;
            }
        }
    };
}

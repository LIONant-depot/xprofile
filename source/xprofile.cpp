// xprofile's shared library: the one profiler client of the process (Tracy's, compiled in here: TracyClient.cpp is a source of this
// target) and the switch every module's macros read. See xprofile.h.
#include "xprofile.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <vector>
#include <deque>
#include <map>
#include <tuple>
#include <string_view>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>

#if defined(_WIN32)
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#else
    #include <pthread.h>
    #include <sys/wait.h>
    #include <unistd.h>
#endif

namespace xprofile
{
    namespace
    {
        std::atomic<bool>   s_bEnabled  { false };
        std::atomic<bool>   s_bStarted  { false };
        std::once_flag      s_Start;
    }

    bool isEnabled(void) noexcept
    {
        return s_bEnabled.load(std::memory_order_relaxed);
    }

    void Enable(bool bOn) noexcept
    {
        if (bOn)
        {
            // the client starts on the first enable (Tracy's manual lifetime) and stops when the process ends - never before: a zone
            // that began while on must still find it when it ends
            std::call_once(s_Start, []
            {
                tracy::StartupProfiler();
                s_bStarted.store(true);
                std::atexit([] { s_bEnabled.store(false); tracy::ShutdownProfiler(); });
            });
        }
        s_bEnabled.store(bOn);
    }

    bool isConnected(void) noexcept
    {
        return s_bStarted.load() && tracy::GetProfiler().IsConnected();
    }

    namespace
    {
        // the copies (std::deque: what was handed out never moves), one per distinct site / name
        std::mutex                                                          s_InternLock;
        std::deque<std::string>                                             s_Strings;
        std::deque<tracy::SourceLocationData>                               s_Locations;
        std::map<std::tuple<std::string, std::string, std::string, std::uint32_t>, const tracy::SourceLocationData*> s_LocationOf;
        std::map<std::string, const char*, std::less<>>                     s_NameOf;

        const char* Keep(const char* p) { return s_Strings.emplace_back(p ? p : "").c_str(); }     // (s_InternLock held)
    }

    const tracy::SourceLocationData* Intern(const char* pName, const char* pFunction, const char* pFile, std::uint32_t Line) noexcept
    {
        std::lock_guard Lk(s_InternLock);
        auto Key = std::make_tuple(std::string(pName ? pName : ""), std::string(pFunction ? pFunction : ""), std::string(pFile ? pFile : ""), Line);
        if (auto It = s_LocationOf.find(Key); It != s_LocationOf.end()) return It->second;
        const auto* p = &s_Locations.emplace_back(tracy::SourceLocationData{ Keep(pName), Keep(pFunction), Keep(pFile), Line, 0 });
        s_LocationOf.emplace(std::move(Key), p);
        return p;
    }

    const tracy::SourceLocationData* InternDynamic(const char* pName) noexcept
    {
        return Intern(pName, "(run-time name)", "", 0);
    }

    const char* InternName(const char* pName) noexcept
    {
        std::lock_guard Lk(s_InternLock);
        if (auto It = s_NameOf.find(std::string_view{ pName }); It != s_NameOf.end()) return It->second;
        const char* p = Keep(pName);
        s_NameOf.emplace(pName, p);
        return p;
    }

    namespace
    {
        thread_local std::string    t_ThreadName;
        thread_local bool           t_bTracyHasName = false;
    }

    void SetThreadName(const char* pName) noexcept
    {
        // the OS name always (debuggers; Tracy reads it for threads named before it started), Tracy's own once it runs
    #if defined(_WIN32)
        const std::wstring Wide(pName, pName + std::char_traits<char>::length(pName));
        SetThreadDescription(GetCurrentThread(), Wide.c_str());
    #elif defined(__APPLE__)
        pthread_setname_np(pName);
    #else
        char Short[16] = {};                                        // (Linux: 15 characters at most)
        for (int i = 0; i < 15 && pName[i]; ++i) Short[i] = pName[i];
        pthread_setname_np(pthread_self(), Short);
    #endif
        // Tracy's own list only takes names once its client runs: a thread named before (xscheduler's workers start with the engine)
        // keeps its name and hands it over at its first zone after the start (RecordZone)
        t_ThreadName = pName;
        t_bTracyHasName = s_bStarted.load();
        if (t_bTracyHasName) tracy::SetThreadName(pName);
    }

    //------------------------------------------------------------------------------
    // The recorder: per thread a ring of the zones that ended (written by its thread only: an entry first, then the head, released; the
    // reader takes what is below the head, acquired), the frames' ends (the thread that marks frames), the plots (a lock: they are few)
    // ponytail: a ring entry can be overwritten while Snapshot reads it if a thread ends 16K zones meanwhile - a wrong row in a table,
    // never a crash (plain data)
    namespace
    {
        struct zone_rec { const location* m_pWhere; std::int64_t m_Start, m_End; };
        struct thread_ring
        {
            static constexpr std::uint64_t  size_v = 1 << 14;
            std::array<zone_rec, size_v>    m_Recs  {};
            std::atomic<std::uint64_t>      m_Head  { 0 };
        };
        std::mutex                                  s_RingsLock;
        std::vector<std::unique_ptr<thread_ring>>   s_Rings;                // never freed: a thread that ends leaves its zones for the window
        thread_local thread_ring*                   t_pRing = nullptr;

        constexpr int                               frames_v = 1024;
        std::array<std::int64_t, frames_v>          s_FrameEnds {};
        std::atomic<std::uint64_t>                  s_nFrames   { 0 };

        struct plot_ring { std::array<float, 256> m_Values{}; std::uint32_t m_n = 0; double m_Last = 0; };
        std::mutex                                  s_PlotsLock;
        std::map<const char*, plot_ring>            s_Plots;                // by interned name
    }

    void RecordZone(const location* pWhere, std::int64_t StartNs, std::int64_t EndNs) noexcept
    {
        if (!pWhere) return;
        if (!t_bTracyHasName && !t_ThreadName.empty() && s_bStarted.load()) { tracy::SetThreadName(t_ThreadName.c_str()); t_bTracyHasName = true; }
        if (!t_pRing)
        {
            std::lock_guard Lk(s_RingsLock);
            t_pRing = s_Rings.emplace_back(std::make_unique<thread_ring>()).get();
        }
        const auto H = t_pRing->m_Head.load(std::memory_order_relaxed);
        t_pRing->m_Recs[H & (thread_ring::size_v - 1)] = { pWhere, StartNs, EndNs };
        t_pRing->m_Head.store(H + 1, std::memory_order_release);
    }

    void RecordFrame(void) noexcept
    {
        const auto n = s_nFrames.load(std::memory_order_relaxed);
        s_FrameEnds[n % frames_v] = Now();
        s_nFrames.store(n + 1, std::memory_order_release);
    }

    void RecordPlot(const char* pInternedName, double Value) noexcept
    {
        std::lock_guard Lk(s_PlotsLock);
        auto& P = s_Plots[pInternedName];
        P.m_Values[P.m_n++ % P.m_Values.size()] = static_cast<float>(Value);
        P.m_Last = Value;
    }

    void ClearRecorder(void) noexcept
    {
        { std::lock_guard Lk(s_PlotsLock); s_Plots.clear(); }
        s_nFrames.store(0);
        std::lock_guard Lk(s_RingsLock);
        for (auto& R : s_Rings) R->m_Head.store(0);       // (a thread writing meanwhile: at worst one stale entry)
    }

    void Snapshot(int nFrames, snapshot& Out) noexcept
    {
        Out.m_FrameMs.clear(); Out.m_Zones.clear(); Out.m_Plots.clear(); Out.m_nThreads = 0;

        // the window: the last nFrames frames (their ends), or the last second when no frame was marked
        const auto nAll = s_nFrames.load(std::memory_order_acquire);
        const int  n    = static_cast<int>(std::min<std::uint64_t>({ nAll ? nAll - 1 : 0, static_cast<std::uint64_t>(std::max(nFrames, 1)), frames_v - 1 }));
        std::int64_t From = Now() - 1'000'000'000, To = Now();
        if (n > 0)
        {
            From = s_FrameEnds[(nAll - 1 - n) % frames_v];
            To   = s_FrameEnds[(nAll - 1) % frames_v];
            for (int i = n; i > 0; --i) Out.m_FrameMs.push_back(static_cast<float>((s_FrameEnds[(nAll - i) % frames_v] - s_FrameEnds[(nAll - i - 1) % frames_v]) * 1e-6));
        }

        // every thread's zones that ended in the window, summed by location
        std::map<const location*, zone_stat> Sum;
        {
            std::lock_guard Lk(s_RingsLock);
            for (auto& R : s_Rings)
            {
                const auto H = R->m_Head.load(std::memory_order_acquire);
                bool bAny = false;
                for (std::uint64_t i = H; i > 0 && H - i < thread_ring::size_v; --i)
                {
                    const auto& Z = R->m_Recs[(i - 1) & (thread_ring::size_v - 1)];
                    if (Z.m_End < From) break;
                    if (Z.m_End > To || !Z.m_pWhere) continue;
                    auto& S = Sum[Z.m_pWhere];
                    const double Ms = (Z.m_End - Z.m_Start) * 1e-6;
                    S.m_pWhere = Z.m_pWhere; ++S.m_Count; S.m_TotalMs += Ms; S.m_MaxMs = std::max(S.m_MaxMs, Ms);
                    bAny = true;
                }
                Out.m_nThreads += bAny;
            }
        }
        for (auto& [p, S] : Sum) Out.m_Zones.push_back(S);
        std::sort(Out.m_Zones.begin(), Out.m_Zones.end(), [](const zone_stat& A, const zone_stat& B) { return A.m_TotalMs > B.m_TotalMs; });

        // the plots: the last value, the range and the history of what was kept
        std::lock_guard Lk(s_PlotsLock);
        for (auto& [pName, P] : s_Plots)
        {
            plot_stat S{ .m_pName = pName, .m_Last = P.m_Last };
            const auto k = std::min<std::uint32_t>(P.m_n, static_cast<std::uint32_t>(P.m_Values.size()));
            for (std::uint32_t i = P.m_n - k; i < P.m_n; ++i) S.m_History.push_back(P.m_Values[i % P.m_Values.size()]);
            if (!S.m_History.empty()) { auto [Lo, Hi] = std::minmax_element(S.m_History.begin(), S.m_History.end()); S.m_Min = *Lo; S.m_Max = *Hi; }
            Out.m_Plots.push_back(std::move(S));
        }
    }

    //------------------------------------------------------------------------------

    bool LaunchViewer(const char* pViewerExe) noexcept
    {
        // detached: the viewer outlives nothing of ours; "-a 127.0.0.1": it connects to this process at once
    #if defined(_WIN32)
        std::string Cmd = std::string("\"") + pViewerExe + "\" -a 127.0.0.1";
        std::wstring WCmd(Cmd.begin(), Cmd.end());
        STARTUPINFOW Si{ .cb = sizeof(STARTUPINFOW) };
        PROCESS_INFORMATION Pi{};
        if (!CreateProcessW(nullptr, WCmd.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &Si, &Pi)) return false;
        CloseHandle(Pi.hThread); CloseHandle(Pi.hProcess);
        return true;
    #else
        const pid_t Pid = fork();
        if (Pid < 0) return false;
        if (Pid == 0)
        {
            setsid();
            if (fork() == 0) { execl(pViewerExe, pViewerExe, "-a", "127.0.0.1", static_cast<char*>(nullptr)); _exit(127); }
            _exit(0);
        }
        int Status = 0; waitpid(Pid, &Status, 0);
        return true;
    #endif
    }

    std::string ViewerPath(void) noexcept
    {
        std::error_code Ec;
        std::filesystem::path Exe;
    #if defined(_WIN32)
        wchar_t Buf[1024] = {};
        if (GetModuleFileNameW(nullptr, Buf, 1024)) Exe = Buf;
        const char* pName = "tracy-profiler.exe";
    #else
        Exe = std::filesystem::canonical("/proc/self/exe", Ec);
        const char* pName = "tracy-profiler";
    #endif
        for (auto Dir = Exe.parent_path(); !Dir.empty(); Dir = Dir.parent_path())
        {
            for (const auto& Sub : { std::filesystem::path("Build") / "tracy" / "profiler", std::filesystem::path("tracy") / "profiler" })
                if (auto P = Dir / Sub / pName; std::filesystem::exists(P, Ec)) return P.string();
            if (Dir == Dir.parent_path()) break;
        }
        return {};
    }
}

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

        struct plot_ring { std::array<float, 256> m_Values{}; std::uint32_t m_n = 0; double m_Last = 0, m_Limit = 0; };
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

    void SetPlotLimit(const char* pInternedName, double Max) noexcept
    {
        std::lock_guard Lk(s_PlotsLock);
        s_Plots[pInternedName].m_Limit = Max;
    }

    void ClearRecorder(void) noexcept
    {
        { std::lock_guard Lk(s_PlotsLock); for (auto& [p, P] : s_Plots) { P.m_n = 0; P.m_Last = 0; } }     // (the limits stay)
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
            if (P.m_n == 0) continue;                       // (a limit set, nothing plotted yet)
            plot_stat S{ .m_pName = pName, .m_Last = P.m_Last, .m_Limit = P.m_Limit };
            const auto k = std::min<std::uint32_t>(P.m_n, static_cast<std::uint32_t>(P.m_Values.size()));
            for (std::uint32_t i = P.m_n - k; i < P.m_n; ++i) S.m_History.push_back(P.m_Values[i % P.m_Values.size()]);
            if (!S.m_History.empty()) { auto [Lo, Hi] = std::minmax_element(S.m_History.begin(), S.m_History.end()); S.m_Min = *Lo; S.m_Max = *Hi; }
            Out.m_Plots.push_back(std::move(S));
        }
    }

    //------------------------------------------------------------------------------

    namespace
    {
        // a program started detached (it outlives nothing of ours): its executable and arguments
        bool Spawn(const std::string& Exe, const std::vector<std::string>& Args) noexcept
        {
        #if defined(_WIN32)
            std::string Cmd = "\"" + Exe + "\"";
            for (auto& A : Args) Cmd += " \"" + A + "\"";
            std::wstring WCmd(Cmd.begin(), Cmd.end());
            STARTUPINFOW Si{ .cb = sizeof(STARTUPINFOW) };
            PROCESS_INFORMATION Pi{};
            if (!CreateProcessW(nullptr, WCmd.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &Si, &Pi)) return false;
            CloseHandle(Pi.hThread); CloseHandle(Pi.hProcess);
            return true;
        #else
            std::vector<char*> Argv{ const_cast<char*>(Exe.c_str()) };
            for (auto& A : Args) Argv.push_back(const_cast<char*>(A.c_str()));
            Argv.push_back(nullptr);
            const pid_t Pid = fork();
            if (Pid < 0) return false;
            if (Pid == 0)
            {
                setsid();
                if (fork() == 0) { execv(Exe.c_str(), Argv.data()); _exit(127); }
                _exit(0);
            }
            int Status = 0; waitpid(Pid, &Status, 0);
            return true;
        #endif
        }
    }

    //------------------------------------------------------------------------------------------------------------
    // Memory: pools -> regions -> allocations by offset, always kept; Tracy's memory window gets the allocations made while connected
    // (each under a made-up address, unique per region and offset)

    namespace
    {
        struct mem_alloc_rec  { std::uint64_t m_Size = 0; const char* m_pTag = nullptr; const char* m_pOwner = nullptr; bool m_bTracy = false; };
        thread_local const char*                    t_pMemOwner = nullptr;
        struct mem_region_rec { std::uint64_t m_Size = 0; std::map<std::uint64_t, mem_alloc_rec> m_Allocs; };
        struct mem_pool_rec   { std::uint64_t m_Used = 0, m_Peak = 0; std::map<std::uint64_t, mem_region_rec> m_Regions; };
        std::mutex                                  s_MemLock;
        std::map<const char*, mem_pool_rec>         s_Mem;                  // by interned name

        void* TracyAddress(std::uint64_t Region, std::uint64_t Offset) noexcept { return reinterpret_cast<void*>(static_cast<std::uintptr_t>(Region * 0x9E3779B97F4A7C15ull + Offset)); }

        void ForgetAlloc(const char* pPool, mem_pool_rec& P, std::uint64_t Region, std::uint64_t Offset, const mem_alloc_rec& A) noexcept
        {
            P.m_Used -= A.m_Size;
            if (A.m_bTracy) TracyFreeN(TracyAddress(Region, Offset), pPool);
        }
    }

    void        SetMemOwner(const char* pName) noexcept { t_pMemOwner = pName ? InternName(pName) : nullptr; }
    const char* getMemOwner(void) noexcept { return t_pMemOwner; }

    void MemRegion(const char* pPool, std::uint64_t Region, std::uint64_t Size) noexcept
    {
        pPool = InternName(pPool);
        std::lock_guard Lk(s_MemLock);
        auto& P = s_Mem[pPool];
        if (Size) { P.m_Regions[Region].m_Size = Size; return; }
        if (auto It = P.m_Regions.find(Region); It != P.m_Regions.end())
        {
            for (auto& [Offset, A] : It->second.m_Allocs) ForgetAlloc(pPool, P, Region, Offset, A);
            P.m_Regions.erase(It);
        }
    }

    void MemAlloc(const char* pPool, std::uint64_t Region, std::uint64_t Offset, std::uint64_t Size, const char* pTag) noexcept
    {
        pPool = InternName(pPool);
        pTag  = pTag ? InternName(pTag) : nullptr;
        std::lock_guard Lk(s_MemLock);
        auto& P = s_Mem[pPool];
        auto& A = P.m_Regions[Region].m_Allocs[Offset];
        if (A.m_Size) ForgetAlloc(pPool, P, Region, Offset, A);            // (a free never told: replaced)
        A = { Size, pTag, t_pMemOwner, s_bStarted.load() && isConnected() };
        if (A.m_bTracy) TracyAllocN(TracyAddress(Region, Offset), Size, pPool);
        P.m_Used += Size;
        P.m_Peak  = std::max(P.m_Peak, P.m_Used);
    }

    void MemFree(const char* pPool, std::uint64_t Region, std::uint64_t Offset) noexcept
    {
        pPool = InternName(pPool);
        std::lock_guard Lk(s_MemLock);
        auto& P = s_Mem[pPool];
        auto R = P.m_Regions.find(Region);
        if (R == P.m_Regions.end()) return;
        if (auto It = R->second.m_Allocs.find(Offset); It != R->second.m_Allocs.end())
        {
            ForgetAlloc(pPool, P, Region, Offset, It->second);
            R->second.m_Allocs.erase(It);
        }
    }

    void MemSnapshot(std::vector<mem_pool>& Out) noexcept
    {
        Out.clear();
        std::lock_guard Lk(s_MemLock);
        for (auto& [pName, P] : s_Mem)
        {
            auto& O = Out.emplace_back();
            O.m_pName = pName; O.m_Used = P.m_Used; O.m_Peak = P.m_Peak;
            for (auto& [Id, R] : P.m_Regions)
            {
                auto& OR = O.m_Regions.emplace_back();
                OR.m_Id = Id; OR.m_Size = R.m_Size;
                std::uint64_t End = 0;
                for (auto& [Offset, A] : R.m_Allocs)
                {
                    OR.m_Allocs.push_back({ Offset, A.m_Size, A.m_pTag, A.m_pOwner });
                    OR.m_Used += A.m_Size;
                    if (Offset > End) OR.m_LargestFree = std::max(OR.m_LargestFree, Offset - End);
                    End = std::max(End, Offset + A.m_Size);
                }
                if (R.m_Size > End) OR.m_LargestFree = std::max(OR.m_LargestFree, R.m_Size - End);
                O.m_Reserved += R.m_Size;
                O.m_nAllocs  += static_cast<std::uint32_t>(R.m_Allocs.size());
            }
        }
    }

    bool LaunchViewer(const char* pViewerExe) noexcept
    {
        // "-a 127.0.0.1": it connects to this process at once
        return Spawn(pViewerExe, { "-a", "127.0.0.1" });
    }

    bool LaunchCapture(const char* pTraceFile, int Seconds) noexcept
    {
        // Tracy's capture tool, no window: this process for Seconds into pTraceFile (-f: overwrite); the profiler must be on
        const auto Exe = ToolPath("capture", "tracy-capture");
        if (Exe.empty()) return false;
        if (!isEnabled()) Enable(true);
        return Spawn(Exe, { "-a", "127.0.0.1", "-o", pTraceFile, "-s", std::to_string(std::max(1, Seconds)), "-f" });
    }

    std::string ToolPath(const char* pFolder, const char* pName) noexcept
    {
        std::error_code Ec;
        std::filesystem::path Exe;
    #if defined(_WIN32)
        wchar_t Buf[1024] = {};
        if (GetModuleFileNameW(nullptr, Buf, 1024)) Exe = Buf;
        const std::string Name = std::string(pName) + ".exe";
    #else
        Exe = std::filesystem::canonical("/proc/self/exe", Ec);
        const std::string Name = pName;
    #endif
        for (auto Dir = Exe.parent_path(); !Dir.empty(); Dir = Dir.parent_path())
        {
            for (const auto& Sub : { std::filesystem::path("Build") / "tracy" / pFolder, std::filesystem::path("tracy") / pFolder })
                if (auto P = Dir / Sub / Name; std::filesystem::exists(P, Ec)) return P.string();
            if (Dir == Dir.parent_path()) break;
        }
        return {};
    }

    std::string ViewerPath(void) noexcept
    {
        return ToolPath("profiler", "tracy-profiler");
    }
}

// xprofile's shared library: the one profiler client of the process (Tracy's, compiled in here: TracyClient.cpp is a source of this
// target) and the switch every module's macros read. See xprofile.h.
#include "xprofile.h"
#include <atomic>
#include <deque>
#include <map>
#include <tuple>
#include <string_view>
#include <cstdlib>
#include <mutex>
#include <string>

#if defined(_WIN32)
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#else
    #include <pthread.h>
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

    const char* InternName(const char* pName) noexcept
    {
        std::lock_guard Lk(s_InternLock);
        if (auto It = s_NameOf.find(std::string_view{ pName }); It != s_NameOf.end()) return It->second;
        const char* p = Keep(pName);
        s_NameOf.emplace(pName, p);
        return p;
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
        if (s_bStarted.load()) tracy::SetThreadName(pName);
    }
}

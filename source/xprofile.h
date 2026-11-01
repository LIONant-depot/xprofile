#ifndef XPROFILE_H
#define XPROFILE_H
#pragma once

// xprofile - the engine's profiling layer: engine code uses these macros, never Tracy's (https://github.com/wolfpld/tracy).
//
//  XPROFILE_SCOPE("name")              a zone, to the end of the scope
//  XPROFILE_SCOPE_TEXT("name", s, n)   the same, with a text shown on the zone (a resource name, a size)
//  XPROFILE_FRAME()                    the end of a frame
//  XPROFILE_PLOT("name", value)        a value over time (MB/s, bytes in flight)
//  XPROFILE_MESSAGE(text, length)      a line on the timeline (the log)
//  XPROFILE_THREAD("name")             names the calling thread (the OS name too: debuggers, Tracy)
//  XPROFILE_SCOPE_DYNAMIC(pName)       a zone named at run time (a job's name)
//  XPROFILE_GPU_SCOPE(Window, "name")  a GPU zone (xgpu::window between render passes, xgpu::cmd_buffer inside one) - Tracy's Vulkan
//                                      context in xGPU, its results collected at the start of each frame
//  xprofile::sequence S; S.Next(XPROFILE_LOCATION("a")); ... S.Next(XPROFILE_LOCATION("b")); S.End();   zones back to back
//
// A header-only library that is also built without xprofile (tools, tests) includes it this way - the macros are there either way:
//      #if __has_include("xprofile.h")
//          #include "xprofile.h"
//      #elif !defined(XPROFILE_SCOPE)
//          #define XPROFILE_SCOPE(NAME)  ... (the ones it uses, empty)
//      #endif
//
// Built with XLION_PROFILE (the xprofile CMake target defines it): the macros feed one profiler client for the whole process - Tracy's,
// in xprofile's own shared library, which every module imports (an engine of several DLLs, the per-Level copies among them, has one
// client, not one per module). Without XLION_PROFILE they are nothing.
//
// Compiled in is not on: nothing is recorded until xprofile::Enable(true) (the editor's Profiler tab, a command); the client starts
// then (Tracy's manual lifetime) and stays up until the process ends - Enable(false) only stops new zones (a zone that started while on
// still ends cleanly).
//
// Two consumers of the same zones (Flax's pattern, profiler_research.md): Tracy (sent while a viewer is connected) and our own recorder -
// per thread a ring of the zones that ended, the frames' ends and the plots' values - which xprofile::Snapshot sums for the editor's
// Profiler tab and the Profiler command (no viewer needed). GPU zones go to Tracy only.

#include <cstdint>
#include <string>
#include <vector>

#if defined(XLION_PROFILE) && XLION_PROFILE

    #include <chrono>
    #include "tracy/Tracy.hpp"
    #include "tracy/TracyC.h"

    #if defined(_WIN32)
        #if defined(XPROFILE_EXPORTS)
            #define XPROFILE_API __declspec(dllexport)
        #else
            #define XPROFILE_API __declspec(dllimport)
        #endif
    #else
        #define XPROFILE_API __attribute__((visibility("default")))
    #endif

    namespace xprofile
    {
        using location = ::tracy::SourceLocationData;

        XPROFILE_API bool   isEnabled   ( void ) noexcept;              // recording (zones, plots, messages) now
        XPROFILE_API void   Enable      ( bool bOn ) noexcept;          // the first Enable(true) starts the client
        XPROFILE_API bool   isConnected ( void ) noexcept;              // a viewer or a capture is attached
        XPROFILE_API void   SetThreadName( const char* pName ) noexcept;

        // Tracy keeps the pointers of a zone's location (name, function, file) and of a plot's name for the whole capture. A Level's own
        // copies of the engine DLLs are unloaded when it closes: their string literals would dangle. So every call site's location and
        // plot name is copied once into this library (which stays) - the same site in two copies of a DLL is one location, one plot.
        // Once: each call site keeps the result in a function-local static (initialised on its first hit; later hits only test the
        // static's guard - no lock, no copy). XPROFILE_SCOPE_DYNAMIC looks its run-time name up each time (a lock: for coarse zones, jobs).
        XPROFILE_API const location*    Intern          ( const char* pName, const char* pFunction, const char* pFile, std::uint32_t Line ) noexcept;
        XPROFILE_API const location*    InternDynamic   ( const char* pName ) noexcept;
        XPROFILE_API const char*        InternName      ( const char* pName ) noexcept;

        // The recorder (the macros call these; nanoseconds of std::chrono::steady_clock)
        XPROFILE_API void   RecordZone  ( const location* pWhere, std::int64_t StartNs, std::int64_t EndNs ) noexcept;
        XPROFILE_API void   RecordFrame ( void ) noexcept;
        XPROFILE_API void   RecordPlot  ( const char* pInternedName, double Value ) noexcept;
        XPROFILE_API void   SetPlotLimit( const char* pInternedName, double Max ) noexcept;     // the theoretical maximum (the tab's Max scale); 0: none
        inline std::int64_t Now         ( void ) noexcept { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

        // What the recorder saw over the last frames
        struct zone_stat
        {
            const location*     m_pWhere    = nullptr;
            std::uint32_t       m_Count     = 0;            // in the window
            double              m_TotalMs   = 0;
            double              m_MaxMs     = 0;
        };
        struct plot_stat
        {
            const char*         m_pName     = nullptr;
            double              m_Last      = 0, m_Min = 0, m_Max = 0;
            double              m_Limit     = 0;            // its theoretical maximum (XPROFILE_PLOT_LIMIT), 0: none
            std::vector<float>  m_History;                  // oldest first
        };
        struct snapshot
        {
            std::vector<float>      m_FrameMs;              // the frames of the window, oldest first
            std::vector<zone_stat>  m_Zones;                // every thread's zones that ended in the window, the most total time first
            std::vector<plot_stat>  m_Plots;
            int                     m_nThreads  = 0;
        };
        XPROFILE_API void   Snapshot    ( int nFrames, snapshot& Out ) noexcept;
        XPROFILE_API void   ClearRecorder( void ) noexcept;

        // Tracy's viewer (a program of its own, the same version as the client): started, connecting to this process (127.0.0.1)
        XPROFILE_API bool   LaunchViewer( const char* pViewerExe ) noexcept;
        // where it is when xprofile's BuildTracyTools made it: <a folder above the executable>/Build/tracy/profiler/tracy-profiler(.exe); "": not built
        XPROFILE_API std::string ViewerPath( void ) noexcept;
        // Tracy's tools found the same way (pFolder: "capture", "csvexport"; pName without .exe); "": not built
        XPROFILE_API std::string ToolPath  ( const char* pFolder, const char* pName ) noexcept;
        // Tracy's capture tool started (no window, the profiler switched on): this process for Seconds into pTraceFile
        XPROFILE_API bool   LaunchCapture   ( const char* pTraceFile, int Seconds ) noexcept;

        // Memory (tracy_plan section 15): pools (a kind of memory: "VRAM"), their regions (a block of it, by any unique id) and the
        // allocations in them by offset. The recorder keeps them always - the map of what is allocated and what is free must be whole
        // when someone looks (meant for few, large allocations: VRAM blocks, resource loads; not every new); Tracy gets each allocation
        // made while it is connected (its memory window). Names are interned here: any string.
        XPROFILE_API void   MemRegion   ( const char* pPool, std::uint64_t Region, std::uint64_t Size ) noexcept;     // Size 0: the region goes, with what is left in it (pPool nullptr: from whichever pool has it)
        XPROFILE_API void   MemAlloc    ( const char* pPool, std::uint64_t Region, std::uint64_t Offset, std::uint64_t Size, const char* pTag ) noexcept;
        XPROFILE_API void   MemFree     ( const char* pPool, std::uint64_t Region, std::uint64_t Offset ) noexcept;
        // Who allocates: a name for this thread's allocations while the scope lives (nested scopes: the innermost) - a resource's load sets
        // "Texture 5E1CE0A110000007" so the memory it makes says whose it is
        XPROFILE_API void        SetMemOwner ( const char* pName ) noexcept;            // (interned here; nullptr: none)
        XPROFILE_API const char* getMemOwner ( void ) noexcept;
        struct mem_owner_scope
        {
            explicit mem_owner_scope( const char* pName ) noexcept : m_pPrev{ getMemOwner() } { SetMemOwner(pName); }
            ~mem_owner_scope( void ) noexcept { SetMemOwner(m_pPrev); }
            const char* m_pPrev;
        };
        struct mem_alloc  { std::uint64_t m_Offset = 0, m_Size = 0; const char* m_pTag = nullptr; const char* m_pOwner = nullptr; };
        struct mem_region
        {
            std::uint64_t           m_Id = 0, m_Size = 0, m_Used = 0;
            std::uint64_t           m_LargestFree = 0;          // the biggest hole (with m_Size - m_Used: how fragmented)
            std::vector<mem_alloc>  m_Allocs;                   // by offset
        };
        struct mem_pool
        {
            const char*             m_pName = nullptr;
            std::uint64_t           m_Used = 0, m_Peak = 0, m_Reserved = 0;     // in allocations, the most ever, in regions
            std::uint32_t           m_nAllocs = 0;
            std::vector<mem_region> m_Regions;                  // by id
        };
        XPROFILE_API void   MemSnapshot ( std::vector<mem_pool>& Out ) noexcept;
        // A memory space (an address space: "VRAM", "System RAM") as the driver sees it: its size, this process's budget in it and what this
        // process uses of it (0: not known) - the maps span all of it, not only what the pools reserved
        struct mem_space  { const char* m_pName = nullptr; std::uint64_t m_Size = 0, m_Budget = 0, m_Usage = 0; };
        XPROFILE_API void   MemSpace    ( const char* pSpace, std::uint64_t Size, std::uint64_t Budget, std::uint64_t Usage ) noexcept;
        XPROFILE_API void   MemSpaces   ( std::vector<mem_space>& Out ) noexcept;      // ("System RAM" from the OS: physical RAM, this process's private bytes)
        // A count kept elsewhere (an allocator's bytes and blocks: the editor's new/delete) - totals, no map
        struct mem_counter { const char* m_pName = nullptr; std::int64_t m_Bytes = 0, m_Count = 0; };
        XPROFILE_API void   MemCounter  ( const char* pName, std::int64_t Bytes, std::int64_t Count ) noexcept;
        XPROFILE_API void   MemCounters ( std::vector<mem_counter>& Out ) noexcept;

        // A zone: Tracy's (sent while a viewer is connected) and the recorder's (the start and end times while enabled)
        struct scope
        {
            scope( const location* pWhere, bool bOn = isEnabled() ) noexcept : m_Zone{ pWhere, -1, bOn }, m_pWhere{ pWhere }, m_Start{ bOn ? Now() : 0 } {}
            ~scope( void ) noexcept { if (m_Start) RecordZone(m_pWhere, m_Start, Now()); }
            ::tracy::ScopedZone m_Zone;
            const location*     m_pWhere;
            std::int64_t        m_Start;
        };

        // The recorder's half of a zone alone (XPROFILE_SCOPE_DYNAMIC: Tracy gets its own transient zone)
        struct record_scope
        {
            explicit record_scope( const location* pWhere ) noexcept : m_pWhere{ pWhere }, m_Start{ pWhere ? Now() : 0 } {}
            ~record_scope( void ) noexcept { if (m_Start) RecordZone(m_pWhere, m_Start, Now()); }
            const location* m_pWhere;
            std::int64_t    m_Start;
        };

        // Zones one after the other (a frame's sections): Next ends the current one and starts the next, End (or the destructor) the last
        struct sequence
        {
            void Next( const location* pWhere ) noexcept
            {
                End();
                if (isEnabled()) { m_Ctx = ___tracy_emit_zone_begin(reinterpret_cast<const ___tracy_source_location_data*>(pWhere), 1); m_pWhere = pWhere; m_Start = Now(); }
            }
            void End( void ) noexcept { if (m_pWhere) { ___tracy_emit_zone_end(m_Ctx); RecordZone(m_pWhere, m_Start, Now()); m_pWhere = nullptr; } }
            ~sequence( void ) noexcept { End(); }
            TracyCZoneCtx       m_Ctx       {};
            const location*     m_pWhere    = nullptr;
            std::int64_t        m_Start     = 0;
        };

        // A GPU zone to the end of the scope on a target with BeginGPUZone(const void*) / EndGPUZone() (xgpu::window between render passes,
        // xgpu::cmd_buffer inside one): the GPU's time of the commands recorded meanwhile
        template< typename T >
        struct gpu_scope
        {
            gpu_scope( T& Target, const location* pWhere ) noexcept : m_Target{ Target } { m_Target.BeginGPUZone(pWhere); }
            ~gpu_scope( void ) noexcept { m_Target.EndGPUZone(); }
            T& m_Target;
        };
    }

    #define XPROFILE_CAT2(A, B)                 A##B
    #define XPROFILE_CAT(A, B)                  XPROFILE_CAT2(A, B)
    #define XPROFILE_SCOPE(NAME)                static const ::xprofile::location* const XPROFILE_CAT(xprofile_where_, __LINE__) = ::xprofile::Intern(NAME, __FUNCTION__, __FILE__, static_cast<std::uint32_t>(__LINE__)); \
                                                ::xprofile::scope XPROFILE_CAT(xprofile_zone_, __LINE__)(XPROFILE_CAT(xprofile_where_, __LINE__))
    #define XPROFILE_SCOPE_TEXT(NAME, TXT, LEN) XPROFILE_SCOPE(NAME); XPROFILE_CAT(xprofile_zone_, __LINE__).m_Zone.Text(TXT, LEN)
    // (Tracy: its transient zone, the name copied per zone - an interned location with no file came out as garbage names in the viewer,
    // 2026-10-31, cause not found yet; the recorder: the interned location)
    // (NAME read only while recording: Tracy's transient zone takes strlen of its name even when inactive - every job paid it, and a job whose
    // name was already freed crashed with the profiler off)
    #define XPROFILE_SCOPE_DYNAMIC(NAME)        const bool XPROFILE_CAT(xprofile_on_, __LINE__) = ::xprofile::isEnabled(); ZoneTransientN(XPROFILE_CAT(xprofile_tzone_, __LINE__), XPROFILE_CAT(xprofile_on_, __LINE__) ? (NAME) : "", XPROFILE_CAT(xprofile_on_, __LINE__)); ::xprofile::record_scope XPROFILE_CAT(xprofile_rzone_, __LINE__)(XPROFILE_CAT(xprofile_on_, __LINE__) ? ::xprofile::InternDynamic(NAME) : nullptr)
    #define XPROFILE_FRAME()                    do { if (::xprofile::isEnabled()) { FrameMark; ::xprofile::RecordFrame(); } } while (0)
    #define XPROFILE_PLOT(NAME, VALUE)          do { if (::xprofile::isEnabled()) { static const char* const XPROFILE_CAT(xprofile_plot_, __LINE__) = ::xprofile::InternName(NAME); TracyPlot(XPROFILE_CAT(xprofile_plot_, __LINE__), VALUE); ::xprofile::RecordPlot(XPROFILE_CAT(xprofile_plot_, __LINE__), VALUE); } } while (0)
    // a plot's theoretical maximum (the hardware's: a disk's rated MB/s, all cores' decode) - the Profiler tab's Max scale
    #define XPROFILE_PLOT_LIMIT(NAME, MAX)      do { if (::xprofile::isEnabled()) { static const char* const XPROFILE_CAT(xprofile_plot_, __LINE__) = ::xprofile::InternName(NAME); ::xprofile::SetPlotLimit(XPROFILE_CAT(xprofile_plot_, __LINE__), MAX); } } while (0)
    #define XPROFILE_MESSAGE(TXT, LEN)          do { if (::xprofile::isEnabled()) { TracyMessage(TXT, LEN); } } while (0)
    #define XPROFILE_THREAD(NAME)               ::xprofile::SetThreadName(NAME)
    #define XPROFILE_MEM_REGION(POOL, REGION, SIZE)                 ::xprofile::MemRegion(POOL, REGION, SIZE)
    #define XPROFILE_MEM_ALLOC(POOL, REGION, OFFSET, SIZE, TAG)     ::xprofile::MemAlloc(POOL, REGION, OFFSET, SIZE, TAG)
    #define XPROFILE_MEM_FREE(POOL, REGION, OFFSET)                 ::xprofile::MemFree(POOL, REGION, OFFSET)
    #define XPROFILE_MEM_OWNER(NAME)                                ::xprofile::mem_owner_scope XPROFILE_CAT(xprofile_owner_, __LINE__)(NAME)
    #define XPROFILE_MEM_SPACE(SPACE, SIZE, BUDGET, USAGE)          ::xprofile::MemSpace(SPACE, SIZE, BUDGET, USAGE)
    // a place for xprofile::sequence::Next (a static location: the name a literal)
    #define XPROFILE_LOCATION(NAME)             ([]() noexcept -> const ::xprofile::location* { static const ::xprofile::location* const L = ::xprofile::Intern(NAME, __FUNCTION__, __FILE__, static_cast<std::uint32_t>(__LINE__)); return L; }())
    #define XPROFILE_GPU_SCOPE(TARGET, NAME)    ::xprofile::gpu_scope XPROFILE_CAT(xprofile_gpu_, __LINE__)(TARGET, XPROFILE_LOCATION(NAME))

#else

    namespace xprofile
    {
        inline bool isEnabled   ( void ) noexcept { return false; }
        inline void Enable      ( bool ) noexcept {}
        inline bool isConnected ( void ) noexcept { return false; }
        inline void SetThreadName( const char* ) noexcept {}
        struct location;
        struct sequence { void Next( const location* ) noexcept {} void End( void ) noexcept {} };
    }

    #define XPROFILE_SCOPE_DYNAMIC(NAME)
    #define XPROFILE_LOCATION(NAME)             (static_cast<const ::xprofile::location*>(nullptr))
    #define XPROFILE_GPU_SCOPE(TARGET, NAME)

    #define XPROFILE_SCOPE(NAME)
    #define XPROFILE_SCOPE_TEXT(NAME, TXT, LEN)
    #define XPROFILE_FRAME()                    do {} while (0)
    #define XPROFILE_PLOT(NAME, VALUE)          do {} while (0)
    #define XPROFILE_PLOT_LIMIT(NAME, MAX)      do {} while (0)
    #define XPROFILE_MESSAGE(TXT, LEN)          do {} while (0)
    #define XPROFILE_THREAD(NAME)               do {} while (0)
    #define XPROFILE_MEM_REGION(POOL, REGION, SIZE)                 do {} while (0)
    #define XPROFILE_MEM_ALLOC(POOL, REGION, OFFSET, SIZE, TAG)     do {} while (0)
    #define XPROFILE_MEM_FREE(POOL, REGION, OFFSET)                 do {} while (0)
    #define XPROFILE_MEM_OWNER(NAME)                                do {} while (0)
    #define XPROFILE_MEM_SPACE(SPACE, SIZE, BUDGET, USAGE)          do {} while (0)

#endif
#endif

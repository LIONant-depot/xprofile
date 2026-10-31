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

#if defined(XLION_PROFILE) && XLION_PROFILE

    #include <cstdint>
    #include "tracy/Tracy.hpp"

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
        XPROFILE_API bool   isEnabled   ( void ) noexcept;              // recording (zones, plots, messages) now
        XPROFILE_API void   Enable      ( bool bOn ) noexcept;          // the first Enable(true) starts the client
        XPROFILE_API bool   isConnected ( void ) noexcept;              // a viewer or a capture is attached
        XPROFILE_API void   SetThreadName( const char* pName ) noexcept;

        // Tracy keeps the pointers of a zone's location (name, function, file) and of a plot's name for the whole capture. A Level's own
        // copies of the engine DLLs are unloaded when it closes: their string literals would dangle. So every call site's location and
        // plot name is copied once into this library (which stays) - the same site in two copies of a DLL is one location, one plot.
        // Once: each call site keeps the result in a function-local static (initialised on its first hit; later hits only test the
        // static's guard - no lock, no copy). XPROFILE_SCOPE_DYNAMIC is the exception: a run-time name is copied by Tracy per zone.
        XPROFILE_API const ::tracy::SourceLocationData* Intern      ( const char* pName, const char* pFunction, const char* pFile, std::uint32_t Line ) noexcept;
        XPROFILE_API const char*                        InternName  ( const char* pName ) noexcept;
    }

    #define XPROFILE_CAT2(A, B)                 A##B
    #define XPROFILE_CAT(A, B)                  XPROFILE_CAT2(A, B)
    #define XPROFILE_SCOPE(NAME)                static const ::tracy::SourceLocationData* const XPROFILE_CAT(xprofile_where_, __LINE__) = ::xprofile::Intern(NAME, __FUNCTION__, __FILE__, static_cast<std::uint32_t>(__LINE__)); \
                                                ::tracy::ScopedZone XPROFILE_CAT(xprofile_zone_, __LINE__)(XPROFILE_CAT(xprofile_where_, __LINE__), -1, ::xprofile::isEnabled())
    #define XPROFILE_SCOPE_TEXT(NAME, TXT, LEN) XPROFILE_SCOPE(NAME); ZoneTextV(XPROFILE_CAT(xprofile_zone_, __LINE__), TXT, LEN)
    #define XPROFILE_FRAME()                    do { if (::xprofile::isEnabled()) { FrameMark; } } while (0)
    #define XPROFILE_PLOT(NAME, VALUE)          do { if (::xprofile::isEnabled()) { static const char* const XPROFILE_CAT(xprofile_plot_, __LINE__) = ::xprofile::InternName(NAME); TracyPlot(XPROFILE_CAT(xprofile_plot_, __LINE__), VALUE); } } while (0)
    #define XPROFILE_MESSAGE(TXT, LEN)          do { if (::xprofile::isEnabled()) { TracyMessage(TXT, LEN); } } while (0)
    #define XPROFILE_THREAD(NAME)               ::xprofile::SetThreadName(NAME)
    // a zone named at run time (a job's name): costs a little more than XPROFILE_SCOPE (its location is made per zone)
    #define XPROFILE_SCOPE_DYNAMIC(NAME)        ZoneTransientN(XPROFILE_CAT(xprofile_zone_, __LINE__), NAME, ::xprofile::isEnabled())
    // a place for xprofile::sequence::Next (a static location: the name a literal)
    #define XPROFILE_LOCATION(NAME)             ([]() noexcept -> const ::xprofile::location* { static const ::xprofile::location* const L = ::xprofile::Intern(NAME, __FUNCTION__, __FILE__, static_cast<std::uint32_t>(__LINE__)); return L; }())

    #include "tracy/TracyC.h"
    namespace xprofile
    {
        using location = ::tracy::SourceLocationData;

        // Zones one after the other (a frame's sections): Next ends the current one and starts the next, End (or the destructor) the last
        struct sequence
        {
            void Next( const location* pWhere ) noexcept
            {
                End();
                if (isEnabled()) { m_Ctx = ___tracy_emit_zone_begin(reinterpret_cast<const ___tracy_source_location_data*>(pWhere), 1); m_bOpen = true; }
            }
            void End( void ) noexcept { if (m_bOpen) { ___tracy_emit_zone_end(m_Ctx); m_bOpen = false; } }
            ~sequence( void ) noexcept { End(); }
            TracyCZoneCtx   m_Ctx   {};
            bool            m_bOpen = false;
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
    #define XPROFILE_MESSAGE(TXT, LEN)          do {} while (0)
    #define XPROFILE_THREAD(NAME)               do {} while (0)

#endif
#endif

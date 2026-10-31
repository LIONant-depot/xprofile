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
//
// Built with XLION_PROFILE (the xprofile CMake target defines it): the macros feed one profiler client for the whole process - Tracy's,
// in xprofile's own shared library, which every module imports (an engine of several DLLs, the per-Level copies among them, has one
// client, not one per module). Without XLION_PROFILE they are nothing.
//
// Compiled in is not on: nothing is recorded until xprofile::Enable(true) (the editor's Profiler tab, a command); the client starts
// then (Tracy's manual lifetime) and stays up until the process ends - Enable(false) only stops new zones (a zone that started while on
// still ends cleanly).

#if defined(XLION_PROFILE) && XLION_PROFILE

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
    }

    #define XPROFILE_CAT2(A, B)                 A##B
    #define XPROFILE_CAT(A, B)                  XPROFILE_CAT2(A, B)
    #define XPROFILE_SCOPE(NAME)                ZoneNamedN(XPROFILE_CAT(xprofile_zone_, __LINE__), NAME, ::xprofile::isEnabled())
    #define XPROFILE_SCOPE_TEXT(NAME, TXT, LEN) ZoneNamedN(XPROFILE_CAT(xprofile_zone_, __LINE__), NAME, ::xprofile::isEnabled()); \
                                                ZoneTextV(XPROFILE_CAT(xprofile_zone_, __LINE__), TXT, LEN)
    #define XPROFILE_FRAME()                    do { if (::xprofile::isEnabled()) { FrameMark; } } while (0)
    #define XPROFILE_PLOT(NAME, VALUE)          do { if (::xprofile::isEnabled()) { TracyPlot(NAME, VALUE); } } while (0)
    #define XPROFILE_MESSAGE(TXT, LEN)          do { if (::xprofile::isEnabled()) { TracyMessage(TXT, LEN); } } while (0)
    #define XPROFILE_THREAD(NAME)               ::xprofile::SetThreadName(NAME)

#else

    namespace xprofile
    {
        inline bool isEnabled   ( void ) noexcept { return false; }
        inline void Enable      ( bool ) noexcept {}
        inline bool isConnected ( void ) noexcept { return false; }
        inline void SetThreadName( const char* ) noexcept {}
    }

    #define XPROFILE_SCOPE(NAME)
    #define XPROFILE_SCOPE_TEXT(NAME, TXT, LEN)
    #define XPROFILE_FRAME()                    do {} while (0)
    #define XPROFILE_PLOT(NAME, VALUE)          do {} while (0)
    #define XPROFILE_MESSAGE(TXT, LEN)          do {} while (0)
    #define XPROFILE_THREAD(NAME)               do {} while (0)

#endif
#endif

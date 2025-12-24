#ifndef UTIL_PROFILER_H
#define UTIL_PROFILER_H

// ASCII-only file
// Lightweight host profiler API for DeSmuMe Wii port.
// Provides RAII ScopedTimer and aggregated JSONL dumps to SD.

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
#include <string>
#include <vector>
//#include <atomic>

namespace Profiler {

// Call after filesystem/logger are ready to pre-create common scopes (safe to call multiple times)
void PrecreateScopes();

// Per-scope aggregated counters (use plain integers; atomic ops use GCC builtins)
struct ScopeStats {
    const char * name;
    uint64_t calls;
    uint64_t total_ns;
    uint64_t max_ns;

    // Default constructor so static arrays of ScopeStats can be defined
    ScopeStats() : name(nullptr), calls(0), total_ns(0), max_ns(0) {}

    // Existing constructor used when creating with a name
    ScopeStats(const char *n) : name(n), calls(0), total_ns(0), max_ns(0) {}
};

// Initialize profiler (create scopes, start timers). Safe to call multiple times.
void InitProfiler();

// Shutdown profiler and flush final dump.
void ShutdownProfiler();

// Force an immediate JSONL dump.
void DumpNow();

// Call once per frame (or periodically) to trigger periodic dumps.
// This avoids background threads on the Wii host.
void TickIfNeeded();

// Get or create a named scope. Returned pointer is stable until ShutdownProfiler.
ScopeStats * GetScopeByName(const char *name);

// RAII timer: construct with ScopeStats*, destructor updates atomic counters.
struct ScopedTimer {
	ScopeStats * stats;
	uint64_t start_ns;
	ScopedTimer(ScopeStats *s);
	~ScopedTimer();
};

// --- Compatibility shim for legacy callers (e.g., Profiler::Instance().SetEnabled(...)) ---
struct LegacyProfiler {
	bool enabled;
	LegacyProfiler();
	void SetEnabled(bool e);
	bool IsEnabled() const;
	void DumpToSDLogger();
};

// Return a reference to the singleton legacy object.
// Usage in existing code: Profiler::Instance().SetEnabled(true);
LegacyProfiler & Instance();

// Safe PROFILE_SCOPE: no function-local static initialization
#define PROFILE_SCOPE(name_literal) \
    do { \
        Profiler::ScopeStats * _prof_scope = Profiler::GetScopeByName(name_literal); \
        Profiler::ScopedTimer _prof_timer(_prof_scope); \
    } while (0)

} // namespace Profiler

extern "C" {
#endif // __cplusplus

// C linkage stubs for C files if needed
void Profiler_InitProfiler();
void Profiler_ShutdownProfiler();
void Profiler_DumpNow();
void Profiler_TickIfNeeded();

#ifdef __cplusplus
} // extern "C"
#endif

#endif // UTIL_PROFILER_H

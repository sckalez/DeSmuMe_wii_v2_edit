// ASCII-only file
// Hardened, allocation-free profiler for DeSmuMe Wii port.
// - No logging calls inside profiler
// - Fixed-size static pool (no heap allocations)
// - Simple spinlock to avoid std::mutex init races

#include "profiler.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h> // for stat, mkdir
#endif

#include <stddef.h>

namespace Profiler {

// Config
static const char * LOG_DIR = "sd:/profiler/";
static const char * LOG_FILE = "sd:/profiler/profiler.log";
static const uint64_t ROTATE_BYTES = 2ULL * 1024ULL * 1024ULL;
static const int MAX_ROTATE = 3;
static const int DUMP_INTERVAL_SEC = 10;
static const int MAX_DUMP_SCOPES = 20;

// Fixed pool size: adjust if you expect >512 distinct scopes
static const size_t FIXED_SCOPE_POOL_SIZE = 512;

// Static pool storage (no dynamic allocation)
static ScopeStats g_fixed_scope_pool[FIXED_SCOPE_POOL_SIZE];
static bool g_fixed_scope_used[FIXED_SCOPE_POOL_SIZE] = {0};
static size_t g_fixed_scope_count = 0;

// Simple container of pointers for iteration (fixed capacity)
static ScopeStats * g_scope_ptrs[FIXED_SCOPE_POOL_SIZE];
static size_t g_scope_ptrs_count = 0;

// Minimal spinlock (uses GCC builtin if available)
static volatile int g_spinlock = 0;
static inline void spin_lock() {
#if defined(__GNUC__) || defined(__clang__)
	while (__sync_lock_test_and_set(&g_spinlock, 1)) { /* busy */ }
#else
	while (__atomic_test_and_set(&g_spinlock, __ATOMIC_ACQUIRE)) { /* busy */ }
#endif
}
static inline void spin_unlock() {
#if defined(__GNUC__) || defined(__clang__)
	__sync_lock_release(&g_spinlock);
#else
	__atomic_clear(&g_spinlock, __ATOMIC_RELEASE);
#endif
}

// Profiler state
static bool g_running = false;
static time_t g_last_dump = 0;

// Monotonic time in ns
static uint64_t now_ns() {
#ifdef _WIN32
	LARGE_INTEGER freq, cnt;
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&cnt);
	return (uint64_t)((cnt.QuadPart * 1000000000ULL) / freq.QuadPart);
#else
	struct timespec ts;
	#if defined(CLOCK_MONOTONIC)
	clock_gettime(CLOCK_MONOTONIC, &ts);
	#else
	struct timeval tv;
	gettimeofday(&tv, NULL);
	ts.tv_sec = tv.tv_sec;
	ts.tv_nsec = tv.tv_usec * 1000;
	#endif
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}

// Get or create a named scope from the fixed pool.
// Returns nullptr if pool exhausted.
ScopeStats * GetScopeByName(const char *name) {
	// Use spinlock to protect pool and pointer list
	spin_lock();

	// Search existing
	for (size_t i = 0; i < g_scope_ptrs_count; ++i) {
		if (strcmp(g_scope_ptrs[i]->name, name) == 0) {
			ScopeStats *found = g_scope_ptrs[i];
			spin_unlock();
			return found;
		}
	}

	// Allocate from fixed pool
	if (g_fixed_scope_count >= FIXED_SCOPE_POOL_SIZE) {
		// pool exhausted
		spin_unlock();
		return nullptr;
	}

	// placement: initialize next slot
	ScopeStats *s = &g_fixed_scope_pool[g_fixed_scope_count];
	// placement-new semantics: call constructor-like init (we have trivial fields)
	s->name = name;
	s->calls = 0;
	s->total_ns = 0;
	s->max_ns = 0;

	g_fixed_scope_used[g_fixed_scope_count] = true;
	g_fixed_scope_count++;

	// add pointer to list
	g_scope_ptrs[g_scope_ptrs_count++] = s;

	spin_unlock();
	return s;
}

// RAII timer: cheap, no logging
ScopedTimer::ScopedTimer(ScopeStats *s) : stats(s), start_ns(now_ns()) { }

ScopedTimer::~ScopedTimer() {
	if (!stats) return;
	uint64_t end = now_ns();
	uint64_t elapsed = (end > start_ns) ? (end - start_ns) : 0;

	// update counters under spinlock to avoid atomics
	spin_lock();
	stats->calls += 1ULL;
	stats->total_ns += elapsed;
	if (elapsed > stats->max_ns) stats->max_ns = elapsed;
	spin_unlock();
}

// Ensure log directory exists (best-effort). Keep minimal and safe.
static void ensure_log_dir() {
	// On Wii host, mkdir may be available; keep best-effort but no logging.
#ifdef _WIN32
	(void)0;
#else
	char tmp[256];
	size_t len = strlen(LOG_DIR);
	if (len == 0 || len >= sizeof(tmp)) return;
	strncpy(tmp, LOG_DIR, sizeof(tmp) - 1);
	tmp[sizeof(tmp) - 1] = 0;
	for (size_t i = 1; i < len; ++i) {
		if (tmp[i] == '/') {
			tmp[i] = 0;
			mkdir(tmp, 0755);
			tmp[i] = '/';
		}
	}
	mkdir(tmp, 0755);
#endif
}

// Rotate logs (best-effort)
static void rotate_logs_if_needed() {
	struct stat st;
	if (stat(LOG_FILE, &st) != 0) return;
	if ((uint64_t)st.st_size <= ROTATE_BYTES) return;

	char oldname[256];
	char newname[256];

	snprintf(oldname, sizeof(oldname), "%sprofiler.%d.log", LOG_DIR, MAX_ROTATE);
	remove(oldname);

	for (int i = MAX_ROTATE - 1; i >= 0; --i) {
		if (i == 0) {
			snprintf(oldname, sizeof(oldname), "%sprofiler.log", LOG_DIR);
		} else {
			snprintf(oldname, sizeof(oldname), "%sprofiler.%d.log", LOG_DIR, i);
		}
		snprintf(newname, sizeof(newname), "%sprofiler.%d.log", LOG_DIR, i + 1);
		rename(oldname, newname);
	}
}

// Dump aggregated top scopes as JSONL. If fopen fails, skip silently.
void DumpNow() {
	// Snapshot pointers under spinlock
	ScopeStats * snapshot[FIXED_SCOPE_POOL_SIZE];
	size_t snapshot_count = 0;

	spin_lock();
	for (size_t i = 0; i < g_scope_ptrs_count; ++i) {
		snapshot[snapshot_count++] = g_scope_ptrs[i];
	}
	spin_unlock();

	// Copy values locally
	struct StatCopy { const char *name; uint64_t calls; uint64_t total_ns; uint64_t max_ns; };
	StatCopy copies[FIXED_SCOPE_POOL_SIZE];
	size_t copies_count = 0;

	for (size_t i = 0; i < snapshot_count; ++i) {
		ScopeStats *s = snapshot[i];
		copies[copies_count].name = s->name;
		copies[copies_count].calls = s->calls;
		copies[copies_count].total_ns = s->total_ns;
		copies[copies_count].max_ns = s->max_ns;
		copies_count++;
	}

	// compute total
	uint64_t total_ns_all = 0;
	for (size_t i = 0; i < copies_count; ++i) total_ns_all += copies[i].total_ns;

	// sort indices by total_ns descending (simple selection sort for small N)
	size_t idx[FIXED_SCOPE_POOL_SIZE];
	for (size_t i = 0; i < copies_count; ++i) idx[i] = i;
	for (size_t i = 0; i < copies_count; ++i) {
		size_t best = i;
		for (size_t j = i + 1; j < copies_count; ++j) {
			if (copies[idx[j]].total_ns > copies[idx[best]].total_ns) best = j;
		}
		size_t tmp = idx[i]; idx[i] = idx[best]; idx[best] = tmp;
	}

	ensure_log_dir();
	rotate_logs_if_needed();

	FILE *f = fopen(LOG_FILE, "a");
	if (!f) return;

	time_t tnow = time(NULL);
	char timestr[64];
	struct tm tmv;
#ifdef _WIN32
	gmtime_s(&tmv, &tnow);
#else
	gmtime_r(&tnow, &tmv);
#endif
	strftime(timestr, sizeof(timestr), "%Y-%m-%dT%H:%M:%SZ", &tmv);

	int limit = (int)copies_count;
	if (limit > MAX_DUMP_SCOPES) limit = MAX_DUMP_SCOPES;

	for (int i = 0; i < limit; ++i) {
		const StatCopy &sc = copies[idx[i]];
		uint64_t calls = sc.calls;
		uint64_t total_ns = sc.total_ns;
		double total_ms = (double)total_ns / 1000000.0;
		double avg_ms = calls ? (total_ms / (double)calls) : 0.0;
		uint64_t max_ns = sc.max_ns;
		double max_ms = (double)max_ns / 1000000.0;
		double pct = total_ns_all ? ((double)total_ns * 100.0 / (double)total_ns_all) : 0.0;

		fprintf(f,
			"{\"scope\":\"%s\",\"calls\":%llu,\"total_ms\":%.3f,\"avg_ms\":%.6f,\"max_ms\":%.3f,\"pct_total\":%.3f,\"timestamp\":\"%s\"}\n",
			sc.name,
			(unsigned long long)calls,
			total_ms,
			avg_ms,
			max_ms,
			pct,
			timestr
		);
	}

	fclose(f);
	g_last_dump = tnow;
}

// Minimal InitProfiler: mark running only (safe early)
void InitProfiler() {
	if (g_running) return;
	g_running = true;
	g_last_dump = time(NULL);
}

// Precreate scopes: call only after filesystem/logger are ready
void PrecreateScopes() {
	if (!g_running) InitProfiler();

	// Create common scopes (may be called multiple times)
	GetScopeByName("CPU_Frame");
	GetScopeByName("GPU_Draw");
	GetScopeByName("GPU_Flush");
	GetScopeByName("Mem_Read");
	GetScopeByName("Mem_Write");
	GetScopeByName("MMU_Lookup");
	GetScopeByName("Audio_Update");
	GetScopeByName("Input_Poll");
	GetScopeByName("VBlank_Wait");
	GetScopeByName("FileBrowser_Draw");
	GetScopeByName("FileBrowser_Input");
	GetScopeByName("Menu_PickDevice");
	GetScopeByName("Menu_FileBrowser");
}

// Shutdown: do final dump and clear pool usage flags (no frees)
void ShutdownProfiler() {
	if (!g_running) return;
	DumpNow();

	spin_lock();
	// reset pool usage but keep memory intact
	for (size_t i = 0; i < g_fixed_scope_count; ++i) {
		g_fixed_scope_used[i] = false;
	}
	g_fixed_scope_count = 0;
	g_scope_ptrs_count = 0;
	spin_unlock();

	g_running = false;
}

void TickIfNeeded() {
	if (!g_running) return;
	time_t nowt = time(NULL);
	if ((nowt - g_last_dump) >= DUMP_INTERVAL_SEC) {
		DumpNow();
	}
}

// Legacy shim
LegacyProfiler::LegacyProfiler() : enabled(false) {}

void LegacyProfiler::SetEnabled(bool e) {
	if (e && !enabled) {
		InitProfiler();
		enabled = true;
	} else if (!e && enabled) {
		ShutdownProfiler();
		enabled = false;
	}
}

bool LegacyProfiler::IsEnabled() const { return enabled; }
void LegacyProfiler::DumpToSDLogger() { DumpNow(); }
LegacyProfiler & Instance() { static LegacyProfiler inst; return inst; }

} // namespace Profiler

extern "C" {
void Profiler_InitProfiler() { Profiler::InitProfiler(); }
void Profiler_ShutdownProfiler() { Profiler::ShutdownProfiler(); }
void Profiler_DumpNow() { Profiler::DumpNow(); }
void Profiler_TickIfNeeded() { Profiler::TickIfNeeded(); }
}

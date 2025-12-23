#pragma once
// Lightweight host profiler (scaffold)
// Place in source/src/utils/profiler.h

#include <cstdint>
#include <atomic>
#include <chrono>
#include <string>
#include <unordered_map>
#include <mutex>

#include "sd_logger.h"

struct ProfilerStat {
	uint64_t calls;
	double total_ms;
	double max_ms;
	ProfilerStat() : calls(0), total_ms(0.0), max_ms(0.0) {}
};

class Profiler {
public:
	static Profiler & Instance();

	// Record a single sample (ms) for a named scope
	void Record(const std::string &scope, double ms);

	// Dump aggregated JSON Lines to SD (or fallback file) and rotate logs
	void DumpToSDLogger();

	// Toggle profiling on/off
	void SetEnabled(bool e);
	bool IsEnabled() const;

	// RAII helper
	class Scope {
	public:
		Scope(const std::string &name);
		~Scope();
	private:
		std::string m_name;
		std::chrono::high_resolution_clock::time_point m_start;
		bool m_active;
	};

private:
	Profiler();
	~Profiler();
	Profiler(const Profiler &) = delete;
	Profiler & operator=(const Profiler &) = delete;

	// Shared stats map
	std::unordered_map<std::string, ProfilerStat> m_stats;

	// Standard mutex for protecting m_stats; keep critical sections tiny.
	std::mutex m_mutex;

	std::atomic<bool> m_enabled{false};
	std::chrono::high_resolution_clock::time_point m_last_dump;
	const char * m_sd_path;
	const size_t m_rotate_size_bytes;
	const int m_keep_files;
};

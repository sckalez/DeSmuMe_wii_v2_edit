// source/src/utils/profiler.cpp
#include "profiler.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

Profiler &Profiler::Instance() {
	static Profiler inst;
	return inst;
}

Profiler::Profiler()
	: m_enabled(false),
	  m_last_dump(std::chrono::high_resolution_clock::now()),
	  m_sd_path("sd:/profiler/"),
	  m_rotate_size_bytes(2 * 1024 * 1024),
	  m_keep_files(3) {
	// minimal ctor
}

Profiler::~Profiler() {
	if (m_enabled.load()) {
		DumpToSDLogger();
	}
}

void Profiler::SetEnabled(bool on) {
	m_enabled.store(on);
	SDLogger_Log("Profiler: SetEnabled -> %s", on ? "ON" : "OFF");
}

bool Profiler::IsEnabled() const {
	return m_enabled.load();
}

void Profiler::Record(const std::string &name, double ms) {
	// Minimal critical section: update stats quickly, no logging while holding the mutex.
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		auto &s = m_stats[name];
		s.calls += 1;
		s.total_ms += ms;
		s.max_ms = std::max(s.max_ms, ms);
	}
	// Intentionally no SDLogger_Log here to avoid noisy output and potential reentrancy.
}

static std::string TimestampNow() {
	std::time_t t = std::time(nullptr);
	char buf[64];
	std::tm tm{};
#if defined(_MSC_VER)
	gmtime_s(&tm, &t);
#else
	if (gmtime_r(&t, &tm) == nullptr) return std::string();
#endif
	if (std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm) > 0)
		return std::string(buf);
	return std::string();
}

void Profiler::DumpToSDLogger() {
	SDLogger_Log("Profiler: DumpToSDLogger() entry");

	// Acquire lock and swap out stats quickly so we hold the mutex for minimal time.
	std::unordered_map<std::string, ProfilerStat> snapshot;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_stats.empty()) {
			SDLogger_Log("Profiler: DumpToSDLogger() no scopes to dump");
			return;
		}
		snapshot.swap(m_stats);
	}

	// Work on snapshot without holding the mutex.
	double grand_total = 0.0;
	uint64_t grand_calls = 0;
	for (auto &kv : snapshot) {
		grand_total += kv.second.total_ms;
		grand_calls += kv.second.calls;
	}

	std::vector<std::pair<std::string, ProfilerStat>> vec;
	vec.reserve(snapshot.size());
	for (auto &kv : snapshot) vec.push_back(kv);

	std::sort(vec.begin(), vec.end(),
			  [](const std::pair<std::string, ProfilerStat> &a, const std::pair<std::string, ProfilerStat> &b) {
				  return a.second.total_ms > b.second.total_ms;
			  });

	if (vec.size() > 20) vec.resize(20);

	// Build JSON Lines payload
	std::string ts = TimestampNow();
	std::ostringstream out;
	for (size_t i = 0; i < vec.size(); ++i) {
		const std::string &scope = vec[i].first;
		const ProfilerStat &s = vec[i].second;
		double avg = s.calls ? (s.total_ms / static_cast<double>(s.calls)) : 0.0;
		double pct = grand_total ? (s.total_ms / grand_total * 100.0) : 0.0;

		out << "{";
		out << "\"scope\":\"" << scope << "\",";
		out << "\"calls\":" << s.calls << ",";
		out << "\"total_ms\":" << s.total_ms << ",";
		out << "\"avg_ms\":" << avg << ",";
		out << "\"max_ms\":" << s.max_ms << ",";
		out << "\"pct_total\":" << pct << ",";
		out << "\"timestamp\":\"" << ts << "\"";
		out << "}\n";
	}

	std::string payload = out.str();
	if (payload.empty()) {
		SDLogger_Log("Profiler: DumpToSDLogger() built empty payload");
		return;
	}

	// Single write to SD logger (done outside any profiler lock)
	SDLogger_Log("Profiler: DumpToSDLogger() writing %zu bytes", payload.size());
	SDLogger_Log("%s", payload.c_str());
	SDLogger_Log("Profiler: DumpToSDLogger() write complete");
}

// RAII Scope implementation
Profiler::Scope::Scope(const std::string &name)
	: m_name(name),
	  m_start(std::chrono::high_resolution_clock::now()),
	  m_active(true) {
	// No logging here to keep profiler quiet in normal operation.
}

Profiler::Scope::~Scope() {
	if (!m_active) return;
	auto end = std::chrono::high_resolution_clock::now();
	std::chrono::duration<double, std::milli> diff = end - m_start;
	double ms = diff.count();

	// Record the sample (Record handles locking internally)
	Profiler::Instance().Record(m_name, ms);
}

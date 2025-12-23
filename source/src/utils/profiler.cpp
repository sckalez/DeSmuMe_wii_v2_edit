#include "profiler.h"
#include <cstdio>
#include <ctime>
#include <sstream>
#include <algorithm>
#include <fstream>
#include <sys/stat.h>

// Try to include sd_logger if present
#ifdef __has_include
# if __has_include("sd_logger.h")
#  include "sd_logger.h"
#  define HAVE_SDLOGGER 1
# endif
#endif

Profiler & Profiler::Instance() {
    static Profiler inst;
    return inst;
}

Profiler::Profiler()
: m_enabled(false)
, m_last_dump(std::chrono::high_resolution_clock::now())
, m_sd_path("sd:/profiler/")
, m_rotate_size_bytes(2 * 1024 * 1024) // 2 MB
, m_keep_files(3)
{
    // ensure path exists if possible (best-effort)
}

Profiler::~Profiler() {
    if (m_enabled.load()) {
        DumpToSDLogger();
    }
}

void Profiler::SetEnabled(bool e) {
    m_enabled.store(e);
}

bool Profiler::IsEnabled() const {
    return m_enabled.load();
}

void Profiler::Record(const std::string &scope, double ms) {
    if (!m_enabled.load()) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    ProfilerStat &s = m_stats[scope];
    s.calls += 1;
    s.total_ms += ms;
    if (ms > s.max_ms) s.max_ms = ms;
}

static std::string TimestampNow() {
    std::time_t t = std::time(NULL);
    char buf[64];
    std::tm *tm = std::gmtime(&t);
    if (tm) {
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", tm);
        return std::string(buf);
    }
    return std::string();
}

static size_t file_size(const std::string &path) {
    struct stat st;
    if (stat(path.c_str(), &st) == 0) return (size_t)st.st_size;
    return 0;
}

void Profiler::DumpToSDLogger() {
    std::unordered_map<std::string, ProfilerStat> snapshot;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        snapshot = m_stats;
    }
    // compute totals
    double grand_total = 0.0;
    uint64_t grand_calls = 0;
    for (auto &kv : snapshot) {
        grand_total += kv.second.total_ms;
        grand_calls += kv.second.calls;
    }

    // build vector and sort by total_ms desc
    std::vector<std::pair<std::string, ProfilerStat> > vec;
    for (auto &kv : snapshot) vec.push_back(kv);
    std::sort(vec.begin(), vec.end(), [](const std::pair<std::string, ProfilerStat> &a, const std::pair<std::string, ProfilerStat> &b){
        return a.second.total_ms > b.second.total_ms;
    });

    // keep top 20
    if (vec.size() > 20) vec.resize(20);

    // prepare JSONL lines
    std::string ts = TimestampNow();
    std::ostringstream out;
    for (size_t i = 0; i < vec.size(); ++i) {
        const std::string &scope = vec[i].first;
        const ProfilerStat &s = vec[i].second;
        double avg = s.calls ? (s.total_ms / (double)s.calls) : 0.0;
        double pct = grand_total ? (s.total_ms / grand_total * 100.0) : 0.0;
        // JSON object per line
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

	// write to sd logger using your SDLogger API (sd:/profiler/profiler.log)
	const std::string filename = "profiler.log";
	// Ensure SD logger is initialized and append the JSONL block.
	// SDLogger_LogFile writes to sd:/<filename> and prefixes a timestamp (per sd_logger.cpp).
	// Use the logger's formatted API to append the entire buffer as one line block.
	SDLogger_LogFile(filename.c_str(), "%s", out.str().c_str());

	// Rotation: use stat on sd:/profiler/profiler.log to check size and rotate if needed.
	// Build full path and check size (best-effort). This uses the helper file_size() below.
	std::string fullpath = std::string(m_sd_path) + filename;
	size_t sz = file_size(fullpath);
	if (sz > m_rotate_size_bytes) {
		// rotate files: profiler.log -> profiler.log.1, keep up to m_keep_files
		for (int i = m_keep_files - 1; i >= 0; --i) {
			std::ostringstream src, dst;
			if (i == 0) src << fullpath;
			else src << fullpath << "." << i;
			dst << fullpath << "." << (i + 1);
			if (file_size(src.str()) > 0) {
				remove(dst.str().c_str());
				rename(src.str().c_str(), dst.str().c_str());
			}
		}
	}
}
 
// RAII Scope implementation
Profiler::Scope::Scope(const std::string &name)
: m_name(name)
, m_start(std::chrono::high_resolution_clock::now())
, m_active(true)
{
}

Profiler::Scope::~Scope() {
    if (!m_active) return;
    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> diff = end - m_start;
    double ms = diff.count();
    Profiler::Instance().Record(m_name, ms);
}

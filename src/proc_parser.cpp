#include "monitor/proc_parser.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string_view>

namespace fs = std::filesystem;

namespace monitor {
namespace {

template <typename T>
std::optional<T> to_number(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    if (s.empty()) return std::nullopt;

    T value{};
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (ec != std::errc() || ptr != s.data() + s.size()) return std::nullopt;
    return value;
}

std::optional<std::string> read_file(const fs::path& path) {
    std::ifstream in(path);
    if (!in) return std::nullopt;
    // /proc files report size 0, so read until EOF instead of trusting the size.
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

}  // namespace

// /proc/<pid>/stat looks like:   1234 (bash) S 1000 1234 1234 34816 ... 98765 ...
// The name is wrapped in parentheses and may itself contain spaces or ')', so we
// anchor on the FIRST '(' and the LAST ')' instead of splitting on whitespace.
std::optional<ProcStat> parse_stat(const std::string& content) {
    const auto open = content.find('(');
    const auto close = content.rfind(')');
    if (open == std::string::npos || close == std::string::npos || close < open) return std::nullopt;

    const auto pid = to_number<pid_t>(std::string_view(content).substr(0, open));
    if (!pid) return std::nullopt;

    // Fields after the name, numbered from field 3 (state) as in proc(5).
    std::istringstream rest(content.substr(close + 1));
    std::vector<std::string> fields;
    for (std::string token; rest >> token;) fields.push_back(token);

    constexpr std::size_t kStartTimeIndex = 22 - 3;  // field 22 -> index 19
    if (fields.size() <= kStartTimeIndex) return std::nullopt;

    const auto ppid = to_number<pid_t>(fields[1]);
    const auto start = to_number<unsigned long long>(fields[kStartTimeIndex]);
    if (!ppid || !start) return std::nullopt;

    ProcStat stat;
    stat.pid = *pid;
    stat.name = content.substr(open + 1, close - open - 1);
    stat.state = fields[0].empty() ? '?' : fields[0][0];
    stat.ppid = *ppid;
    stat.start_time_ticks = *start;
    return stat;
}

// Looks for a line like "VmRSS:\t    5432 kB" in /proc/<pid>/status.
std::optional<long> parse_vmrss_kb(const std::string& status_content) {
    std::istringstream in(status_content);
    for (std::string line; std::getline(in, line);) {
        if (line.rfind("VmRSS:", 0) != 0) continue;
        std::istringstream value(line.substr(6));
        long kb = 0;
        if (value >> kb) return kb;
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<ProcessInfo> read_process(pid_t pid, const std::string& proc_root) {
    const fs::path dir = fs::path(proc_root) / std::to_string(pid);

    const auto stat_text = read_file(dir / "stat");
    if (!stat_text) return std::nullopt;  // process is gone
    const auto stat = parse_stat(*stat_text);
    if (!stat) return std::nullopt;

    ProcessInfo info;
    info.pid = pid;
    info.ppid = stat->ppid;
    info.state = stat->state;
    info.name = stat->name;
    info.start_time_ticks = stat->start_time_ticks;

    if (const auto status_text = read_file(dir / "status")) {
        if (const auto rss = parse_vmrss_kb(*status_text)) info.rss_kb = *rss;
    }
    return info;
}

std::vector<pid_t> list_pids(const std::string& proc_root) {
    std::vector<pid_t> pids;
    std::error_code ec;
    fs::directory_iterator it(proc_root, ec);
    const fs::directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
        if (const auto pid = to_number<pid_t>(it->path().filename().string())) {
            if (*pid > 0) pids.push_back(*pid);
        }
    }
    std::sort(pids.begin(), pids.end());
    return pids;
}

}  // namespace monitor

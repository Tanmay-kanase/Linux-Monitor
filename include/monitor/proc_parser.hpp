#pragma once

#include <sys/types.h>

#include <optional>
#include <string>
#include <vector>

namespace monitor {

// Fields we need from /proc/<pid>/stat.
struct ProcStat {
    pid_t pid = 0;
    std::string name;  // the "comm" field (may contain spaces and parentheses)
    char state = '?';
    pid_t ppid = 0;
    unsigned long long start_time_ticks = 0;  // field 22; with the pid it identifies a process uniquely
};

struct ProcessInfo {
    pid_t pid = 0;
    pid_t ppid = 0;
    char state = '?';
    std::string name;
    unsigned long long start_time_ticks = 0;
    long rss_kb = 0;  // 0 for kernel threads (no VmRSS line)
};

// Pure parsers (no I/O), so they can be unit-tested with plain strings.
std::optional<ProcStat> parse_stat(const std::string& content);
std::optional<long> parse_vmrss_kb(const std::string& status_content);

// I/O helpers. `proc_root` is injectable so tests can point at a fake /proc.
std::optional<ProcessInfo> read_process(pid_t pid, const std::string& proc_root = "/proc");
std::vector<pid_t> list_pids(const std::string& proc_root = "/proc");

}  // namespace monitor

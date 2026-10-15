#pragma once

#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "monitor/event.hpp"
#include "monitor/event_queue.hpp"
#include "monitor/proc_parser.hpp"

namespace monitor {

struct ProcessMonitorOptions {
    std::chrono::milliseconds poll_interval{500};
    long memory_threshold_kb = 0;   // emit PROCESS_MEMORY_HIGH at/above this RSS; 0 disables
    bool report_existing = false;   // announce processes already running at startup
    std::string proc_root = "/proc";  // injectable for tests
};

// Detects process start/exit (and high memory use) by periodically scanning /proc
// and diffing against the previous snapshot.
//
// Processes are tracked by (pid, start_time) rather than pid alone, so a recycled
// pid is correctly reported as "old process exited, new process started".
//
// Thread-safety: poll_once() is called only by the monitor thread once start() has
// been called. Tests may call it directly *instead of* start().
class ProcessMonitor {
public:
    explicit ProcessMonitor(EventQueue<Event>& queue, ProcessMonitorOptions options = {});
    ~ProcessMonitor();

    ProcessMonitor(const ProcessMonitor&) = delete;
    ProcessMonitor& operator=(const ProcessMonitor&) = delete;

    void start();
    void stop();  // idempotent; blocks until the thread has exited

    // One scan + diff. Public so it can be unit-tested deterministically.
    void poll_once();

private:
    using Key = std::pair<pid_t, unsigned long long>;  // (pid, start_time_ticks)
    struct Tracked {
        ProcessInfo info;
        bool memory_alerted = false;
    };

    void run();
    void emit(EventType type, const ProcessInfo& info);
    void check_memory(Tracked& tracked);

    EventQueue<Event>& queue_;
    ProcessMonitorOptions options_;

    std::map<Key, Tracked> tracked_;
    bool first_poll_ = true;

    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_requested_ = false;
    std::thread thread_;
};

}  // namespace monitor

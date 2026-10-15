#include "monitor/proc_monitor.hpp"

namespace monitor {

ProcessMonitor::ProcessMonitor(EventQueue<Event>& queue, ProcessMonitorOptions options)
    : queue_(queue), options_(std::move(options)) {}

ProcessMonitor::~ProcessMonitor() { stop(); }

void ProcessMonitor::start() {
    if (thread_.joinable()) return;
    thread_ = std::thread(&ProcessMonitor::run, this);
}

void ProcessMonitor::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_requested_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void ProcessMonitor::run() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stop_requested_) {
        lock.unlock();
        poll_once();
        lock.lock();
        // Sleep until the next poll, but wake immediately if stop() is called.
        cv_.wait_for(lock, options_.poll_interval, [this] { return stop_requested_; });
    }
}

void ProcessMonitor::poll_once() {
    std::map<Key, ProcessInfo> current;
    for (const pid_t pid : list_pids(options_.proc_root)) {
        // A process can exit between listing and reading: read_process() then
        // returns nullopt and it is simply treated as already gone.
        if (const auto info = read_process(pid, options_.proc_root)) {
            current.emplace(Key{info->pid, info->start_time_ticks}, *info);
        }
    }

    const bool announce_starts = !first_poll_ || options_.report_existing;

    // 1) Processes we knew about that are no longer there -> exited.
    for (auto it = tracked_.begin(); it != tracked_.end();) {
        if (current.find(it->first) == current.end()) {
            emit(EventType::ProcessExited, it->second.info);  // last known name / RSS
            it = tracked_.erase(it);
        } else {
            ++it;
        }
    }

    // 2) New processes -> started; existing ones -> refresh memory figures.
    for (const auto& [key, info] : current) {
        auto it = tracked_.find(key);
        if (it == tracked_.end()) {
            if (announce_starts) emit(EventType::ProcessStarted, info);
            it = tracked_.emplace(key, Tracked{info, false}).first;
        } else {
            it->second.info = info;
        }
        check_memory(it->second);
    }

    first_poll_ = false;
}

// Fires once when RSS crosses the threshold; re-arms when it drops back below.
void ProcessMonitor::check_memory(Tracked& tracked) {
    if (options_.memory_threshold_kb <= 0) return;
    if (tracked.info.rss_kb >= options_.memory_threshold_kb) {
        if (!tracked.memory_alerted) {
            tracked.memory_alerted = true;
            emit(EventType::ProcessMemoryHigh, tracked.info);
        }
    } else {
        tracked.memory_alerted = false;
    }
}

void ProcessMonitor::emit(EventType type, const ProcessInfo& info) {
    Event event;
    event.type = type;
    event.subject = info.name;
    event.pid = info.pid;
    event.rss_kb = info.rss_kb;
    queue_.push(std::move(event));
}

}  // namespace monitor

#pragma once

#include <sys/types.h>

#include <chrono>
#include <string>

namespace monitor {

enum class EventType {
    FileCreated,
    FileModified,
    FileDeleted,
    FileMovedFrom,
    FileMovedTo,
    ProcessStarted,
    ProcessExited,
    ProcessMemoryHigh,
};

// A single observation produced by a watcher/monitor and consumed by the logger.
struct Event {
    EventType type = EventType::FileCreated;
    std::chrono::system_clock::time_point timestamp = std::chrono::system_clock::now();
    std::string subject;  // file path (file events) or process name (process events)
    pid_t pid = 0;        // process events only
    long rss_kb = 0;      // process events only: resident memory in kB
};

const char* to_string(EventType type) noexcept;
bool is_file_event(EventType type) noexcept;

// "2023-11-14T22:13:20.123Z" (UTC, millisecond precision)
std::string format_timestamp(std::chrono::system_clock::time_point tp);

// One log line, e.g.
//   2023-11-14T22:13:20.123Z FILE_CREATED /tmp/watched/a.txt
//   2023-11-14T22:13:20.123Z PROCESS_STARTED pid=4242 name=bash rss_kb=5120
std::string format_event(const Event& event);

}  // namespace monitor

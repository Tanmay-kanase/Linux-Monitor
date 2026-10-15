#include "monitor/event.hpp"

#include <cstdio>
#include <ctime>
#include <sstream>

namespace monitor {

const char* to_string(EventType type) noexcept {
    switch (type) {
        case EventType::FileCreated:       return "FILE_CREATED";
        case EventType::FileModified:      return "FILE_MODIFIED";
        case EventType::FileDeleted:       return "FILE_DELETED";
        case EventType::FileMovedFrom:     return "FILE_MOVED_FROM";
        case EventType::FileMovedTo:       return "FILE_MOVED_TO";
        case EventType::ProcessStarted:    return "PROCESS_STARTED";
        case EventType::ProcessExited:     return "PROCESS_EXITED";
        case EventType::ProcessMemoryHigh: return "PROCESS_MEMORY_HIGH";
    }
    return "UNKNOWN";
}

bool is_file_event(EventType type) noexcept {
    switch (type) {
        case EventType::FileCreated:
        case EventType::FileModified:
        case EventType::FileDeleted:
        case EventType::FileMovedFrom:
        case EventType::FileMovedTo:
            return true;
        default:
            return false;
    }
}

std::string format_timestamp(std::chrono::system_clock::time_point tp) {
    using namespace std::chrono;
    const auto secs = time_point_cast<seconds>(tp);
    const auto ms = duration_cast<milliseconds>(tp - secs).count();

    const std::time_t t = system_clock::to_time_t(secs);
    std::tm tm{};
    gmtime_r(&t, &tm);

    char date[32];
    std::strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%S", &tm);

    char out[48];
    std::snprintf(out, sizeof out, "%s.%03lldZ", date, static_cast<long long>(ms));
    return out;
}

std::string format_event(const Event& event) {
    std::ostringstream os;
    os << format_timestamp(event.timestamp) << ' ' << to_string(event.type);
    if (is_file_event(event.type)) {
        os << ' ' << event.subject;
    } else {
        os << " pid=" << event.pid << " name=" << event.subject << " rss_kb=" << event.rss_kb;
    }
    return os.str();
}

}  // namespace monitor

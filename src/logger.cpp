#include "monitor/logger.hpp"

namespace monitor {

EventLogger::EventLogger(EventQueue<Event>& queue, std::ostream& out) : queue_(queue), out_(out) {}

EventLogger::~EventLogger() { stop(); }

void EventLogger::start() {
    if (thread_.joinable()) return;
    thread_ = std::thread(&EventLogger::run, this);
}

void EventLogger::stop() {
    queue_.close();
    if (thread_.joinable()) thread_.join();
}

void EventLogger::run() {
    // pop() returns nullopt only once the queue is closed AND empty.
    while (auto event = queue_.pop()) {
        out_ << format_event(*event) << std::endl;  // flush: friendly to `tail -f`
        ++logged_;
    }
}

}  // namespace monitor

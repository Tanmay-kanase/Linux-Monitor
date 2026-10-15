#pragma once

#include <atomic>
#include <cstddef>
#include <ostream>
#include <thread>

#include "monitor/event.hpp"
#include "monitor/event_queue.hpp"

namespace monitor {

// Consumer side of the producer-consumer pipeline: pops events off the queue and
// writes one formatted line per event. Keeping I/O on this thread means a slow
// disk or terminal can never stall inotify reads or /proc scanning.
class EventLogger {
public:
    EventLogger(EventQueue<Event>& queue, std::ostream& out);
    ~EventLogger();

    EventLogger(const EventLogger&) = delete;
    EventLogger& operator=(const EventLogger&) = delete;

    void start();

    // Closes the queue, lets the thread drain what is left, then joins it.
    // Stop the producers first: events pushed after close() are dropped.
    void stop();

    std::size_t events_logged() const { return logged_.load(); }

private:
    void run();

    EventQueue<Event>& queue_;
    std::ostream& out_;
    std::atomic<std::size_t> logged_{0};
    std::thread thread_;
};

}  // namespace monitor

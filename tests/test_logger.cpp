#include <chrono>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

#include "monitor/event.hpp"
#include "monitor/logger.hpp"

using namespace monitor;
using namespace std::chrono;

namespace {
// 1700000000 s since epoch == 2023-11-14T22:13:20Z
system_clock::time_point fixed_time() {
    return system_clock::time_point(seconds(1700000000) + milliseconds(123));
}
}  // namespace

TEST(Event, FormatsTimestampInUtcWithMilliseconds) {
    EXPECT_EQ(format_timestamp(fixed_time()), "2023-11-14T22:13:20.123Z");
}

TEST(Event, FormatsFileEvent) {
    Event e;
    e.type = EventType::FileCreated;
    e.timestamp = fixed_time();
    e.subject = "/tmp/watched/a.txt";
    EXPECT_EQ(format_event(e), "2023-11-14T22:13:20.123Z FILE_CREATED /tmp/watched/a.txt");
}

TEST(Event, FormatsProcessEvent) {
    Event e;
    e.type = EventType::ProcessStarted;
    e.timestamp = fixed_time();
    e.subject = "bash";
    e.pid = 4242;
    e.rss_kb = 5120;
    EXPECT_EQ(format_event(e), "2023-11-14T22:13:20.123Z PROCESS_STARTED pid=4242 name=bash rss_kb=5120");
}

TEST(Event, EveryTypeHasADistinctName) {
    const EventType all[] = {EventType::FileCreated,    EventType::FileModified,   EventType::FileDeleted,
                             EventType::FileMovedFrom,  EventType::FileMovedTo,    EventType::ProcessStarted,
                             EventType::ProcessExited,  EventType::ProcessMemoryHigh};
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); ++i) {
        for (size_t j = i + 1; j < sizeof(all) / sizeof(all[0]); ++j) {
            EXPECT_NE(std::string(to_string(all[i])), std::string(to_string(all[j])));
        }
    }
}

TEST(EventLogger, WritesEveryQueuedEventInOrderAndDrainsOnStop) {
    EventQueue<Event> q;
    std::ostringstream out;
    EventLogger logger(q, out);
    logger.start();

    for (int i = 0; i < 50; ++i) {
        Event e;
        e.type = EventType::FileModified;
        e.timestamp = fixed_time();
        e.subject = "/f" + std::to_string(i);
        q.push(e);
    }
    logger.stop();  // must flush everything that was queued

    EXPECT_EQ(logger.events_logged(), 50u);

    std::istringstream lines(out.str());
    std::string line;
    int n = 0;
    while (std::getline(lines, line)) {
        EXPECT_EQ(line, "2023-11-14T22:13:20.123Z FILE_MODIFIED /f" + std::to_string(n));
        ++n;
    }
    EXPECT_EQ(n, 50);
}

TEST(EventLogger, StopWithoutEventsAndDoubleStopAreSafe) {
    EventQueue<Event> q;
    std::ostringstream out;
    EventLogger logger(q, out);
    logger.start();
    logger.stop();
    logger.stop();
    EXPECT_EQ(logger.events_logged(), 0u);
    EXPECT_TRUE(out.str().empty());
}

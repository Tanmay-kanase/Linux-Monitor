#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "monitor/event_queue.hpp"

using monitor::EventQueue;
using namespace std::chrono_literals;

TEST(EventQueue, PreservesFifoOrder) {
    EventQueue<int> q;
    q.push(1);
    q.push(2);
    q.push(3);
    EXPECT_EQ(q.size(), 3u);
    EXPECT_EQ(*q.pop(), 1);
    EXPECT_EQ(*q.pop(), 2);
    EXPECT_EQ(*q.pop(), 3);
}

TEST(EventQueue, PopForTimesOutOnEmptyQueue) {
    EventQueue<int> q;
    const auto start = std::chrono::steady_clock::now();
    const auto item = q.pop_for(50ms);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_FALSE(item.has_value());
    EXPECT_TRUE(elapsed >= 40ms);
}

TEST(EventQueue, CloseLetsConsumersDrainThenReturnsNullopt) {
    EventQueue<int> q;
    q.push(10);
    q.push(20);
    q.close();
    EXPECT_TRUE(q.closed());
    EXPECT_EQ(*q.pop(), 10);
    EXPECT_EQ(*q.pop(), 20);
    EXPECT_FALSE(q.pop().has_value());
}

TEST(EventQueue, PushAfterCloseIsIgnored) {
    EventQueue<int> q;
    q.close();
    q.push(1);
    EXPECT_EQ(q.size(), 0u);
    EXPECT_FALSE(q.pop().has_value());
}

TEST(EventQueue, BlockedPopWakesUpOnPush) {
    EventQueue<int> q;
    std::optional<int> received;
    std::thread consumer([&] { received = q.pop(); });
    std::this_thread::sleep_for(50ms);  // let the consumer block
    q.push(42);
    consumer.join();
    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(*received, 42);
}

TEST(EventQueue, BlockedPopWakesUpOnClose) {
    EventQueue<int> q;
    std::optional<int> received = 123;
    std::thread consumer([&] { received = q.pop(); });
    std::this_thread::sleep_for(50ms);
    q.close();
    consumer.join();
    EXPECT_FALSE(received.has_value());
}

TEST(EventQueue, ManyProducersManyConsumersLoseNothing) {
    constexpr int kProducers = 4;
    constexpr int kConsumers = 3;
    constexpr int kPerProducer = 2000;

    EventQueue<int> q;
    std::atomic<long long> sum{0};
    std::atomic<int> count{0};

    std::vector<std::thread> consumers;
    for (int c = 0; c < kConsumers; ++c) {
        consumers.emplace_back([&] {
            while (auto v = q.pop()) {
                sum += *v;
                ++count;
            }
        });
    }

    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&] {
            for (int i = 1; i <= kPerProducer; ++i) q.push(i);
        });
    }

    for (auto& t : producers) t.join();
    q.close();
    for (auto& t : consumers) t.join();

    const long long expected_per_producer = static_cast<long long>(kPerProducer) * (kPerProducer + 1) / 2;
    EXPECT_EQ(count.load(), kProducers * kPerProducer);
    EXPECT_EQ(sum.load(), expected_per_producer * kProducers);
}

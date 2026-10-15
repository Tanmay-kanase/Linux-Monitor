#include <unistd.h>

#include <algorithm>
#include <string>

#include <gtest/gtest.h>

#include "monitor/proc_parser.hpp"
#include "test_util.hpp"

using namespace monitor;
using testutil::TempDir;

namespace {
// Fields 4..22 of a real /proc/<pid>/stat line; field 22 (starttime) is 98765.
const char* kTail =
    " S 1000 1234 1234 34816 1234 4194304 1000 0 0 0 5 3 0 0 20 0 1 0 98765 12345678 1000 18446744073709551615";
}  // namespace

TEST(ParseStat, ParsesTypicalLine) {
    const auto stat = parse_stat(std::string("1234 (bash)") + kTail);
    ASSERT_TRUE(stat.has_value());
    EXPECT_EQ(stat->pid, 1234);
    EXPECT_EQ(stat->name, "bash");
    EXPECT_EQ(stat->state, 'S');
    EXPECT_EQ(stat->ppid, 1000);
    EXPECT_EQ(stat->start_time_ticks, 98765ull);
}

TEST(ParseStat, NameMayContainSpacesAndParentheses) {
    const auto stat = parse_stat(std::string("42 (weird (name) x)") + kTail);
    ASSERT_TRUE(stat.has_value());
    EXPECT_EQ(stat->pid, 42);
    EXPECT_EQ(stat->name, "weird (name) x");
    EXPECT_EQ(stat->state, 'S');
    EXPECT_EQ(stat->start_time_ticks, 98765ull);
}

TEST(ParseStat, RejectsMalformedInput) {
    EXPECT_FALSE(parse_stat("").has_value());
    EXPECT_FALSE(parse_stat("garbage").has_value());
    EXPECT_FALSE(parse_stat("abc (bash) S 1 2 3").has_value());       // pid not a number
    EXPECT_FALSE(parse_stat("1 (bash) S 1 2 3").has_value());         // too few fields
    EXPECT_FALSE(parse_stat(std::string("1 bash)") + kTail).has_value());  // no '('
}

TEST(ParseVmRss, ReadsValueInKb) {
    const std::string status =
        "Name:\tbash\nVmPeak:\t   10000 kB\nVmRSS:\t    5432 kB\nThreads:\t1\n";
    const auto rss = parse_vmrss_kb(status);
    ASSERT_TRUE(rss.has_value());
    EXPECT_EQ(*rss, 5432);
}

TEST(ParseVmRss, MissingLineReturnsNullopt) {
    // Kernel threads have no VmRSS line.
    EXPECT_FALSE(parse_vmrss_kb("Name:\tkthreadd\nThreads:\t1\n").has_value());
    EXPECT_FALSE(parse_vmrss_kb("").has_value());
}

TEST(ReadProcess, ReadsFromFakeProcRoot) {
    TempDir root;
    const auto dir = root.path() / "777";
    std::filesystem::create_directories(dir);
    testutil::write_file(dir / "stat", std::string("777 (my daemon)") + kTail + "\n");
    testutil::write_file(dir / "status", "Name:\tmy daemon\nVmRSS:\t2048 kB\n");

    const auto info = read_process(777, root.str());
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->pid, 777);
    EXPECT_EQ(info->name, "my daemon");
    EXPECT_EQ(info->rss_kb, 2048);
    EXPECT_EQ(info->start_time_ticks, 98765ull);
}

TEST(ReadProcess, MissingProcessReturnsNullopt) {
    TempDir root;
    EXPECT_FALSE(read_process(99999, root.str()).has_value());
}

TEST(ReadProcess, MissingStatusMeansZeroRss) {
    TempDir root;
    const auto dir = root.path() / "5";
    std::filesystem::create_directories(dir);
    testutil::write_file(dir / "stat", std::string("5 (kworker)") + kTail + "\n");

    const auto info = read_process(5, root.str());
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->rss_kb, 0);
}

TEST(ListPids, ReturnsOnlyNumericDirectoriesSorted) {
    TempDir root;
    for (const char* name : {"30", "4", "200", "self", "cpuinfo", "12abc"}) {
        std::filesystem::create_directories(root.path() / name);
    }
    const auto pids = list_pids(root.str());
    ASSERT_EQ(pids.size(), 3u);
    EXPECT_EQ(pids[0], 4);
    EXPECT_EQ(pids[1], 30);
    EXPECT_EQ(pids[2], 200);
}

TEST(ListPids, MissingRootGivesEmptyList) {
    EXPECT_TRUE(list_pids("/nonexistent/proc/root").empty());
}

// --- against the real /proc ---

TEST(RealProc, ListsAndReadsOurOwnProcess) {
    const pid_t self = ::getpid();
    const auto pids = list_pids();
    EXPECT_TRUE(std::find(pids.begin(), pids.end(), self) != pids.end());

    const auto info = read_process(self);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->pid, self);
    EXPECT_EQ(info->ppid, ::getppid());
    EXPECT_FALSE(info->name.empty());
    EXPECT_TRUE(info->rss_kb > 0);
}

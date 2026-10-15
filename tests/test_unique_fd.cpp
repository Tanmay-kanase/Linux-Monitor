#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <utility>

#include <gtest/gtest.h>

#include "monitor/unique_fd.hpp"

using monitor::UniqueFd;

namespace {
bool is_open(int fd) { return ::fcntl(fd, F_GETFD) != -1 || errno != EBADF; }
}  // namespace

TEST(UniqueFd, DefaultConstructedIsInvalid) {
    UniqueFd fd;
    EXPECT_FALSE(fd.valid());
    EXPECT_FALSE(static_cast<bool>(fd));
    EXPECT_EQ(fd.get(), -1);
}

TEST(UniqueFd, ClosesDescriptorOnDestruction) {
    int raw = ::dup(STDIN_FILENO);
    ASSERT_TRUE(raw >= 0);
    {
        UniqueFd fd(raw);
        EXPECT_TRUE(fd.valid());
        EXPECT_TRUE(is_open(raw));
    }
    EXPECT_FALSE(is_open(raw));
}

TEST(UniqueFd, MoveConstructionTransfersOwnership) {
    const int raw = ::dup(STDIN_FILENO);
    ASSERT_TRUE(raw >= 0);
    UniqueFd a(raw);
    UniqueFd b(std::move(a));
    EXPECT_FALSE(a.valid());
    EXPECT_EQ(b.get(), raw);
    EXPECT_TRUE(is_open(raw));
}

TEST(UniqueFd, MoveAssignmentClosesPreviousDescriptor) {
    const int first = ::dup(STDIN_FILENO);
    const int second = ::dup(STDIN_FILENO);
    ASSERT_TRUE(first >= 0 && second >= 0);

    UniqueFd a(first);
    UniqueFd b(second);
    a = std::move(b);

    EXPECT_FALSE(is_open(first));  // overwritten fd was closed
    EXPECT_EQ(a.get(), second);
    EXPECT_FALSE(b.valid());
}

TEST(UniqueFd, ReleaseGivesUpOwnershipWithoutClosing) {
    const int raw = ::dup(STDIN_FILENO);
    ASSERT_TRUE(raw >= 0);
    {
        UniqueFd fd(raw);
        EXPECT_EQ(fd.release(), raw);
        EXPECT_FALSE(fd.valid());
    }
    EXPECT_TRUE(is_open(raw));
    ::close(raw);
}

TEST(UniqueFd, ResetReplacesDescriptor) {
    const int first = ::dup(STDIN_FILENO);
    const int second = ::dup(STDIN_FILENO);
    ASSERT_TRUE(first >= 0 && second >= 0);

    UniqueFd fd(first);
    fd.reset(second);
    EXPECT_FALSE(is_open(first));
    EXPECT_EQ(fd.get(), second);
    fd.reset();
    EXPECT_FALSE(is_open(second));
    EXPECT_FALSE(fd.valid());
}

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "shared/base/call_logger.h"

using json = nlohmann::json;

// CallLogger with redis=nullptr �?background flush thread runs harmlessly
class CallLoggerTest : public ::testing::Test {
   protected:
    CallLogger logger{100, nullptr};  // max 100 entries, no redis
};

TEST_F(CallLoggerTest, LogAndGetHistoryBasic) {
    logger.Log("alice", "SheetService", "GetSpreadsheet", json::object(), json::object(), true, 1500);
    logger.Log("bob", "FileService", "UploadFile", json::object(), json::object(), true, 3200);

    auto history = logger.GetHistory(50, 0, "", "");
    ASSERT_EQ(history.size(), 2);
    // Most recent first (reverse chronological)
    EXPECT_EQ(history[0].username, "bob");
    EXPECT_EQ(history[1].username, "alice");
}

TEST_F(CallLoggerTest, GetHistoryRespectsLimit) {
    for (int i = 0; i < 10; ++i)
        logger.Log("user", "svc", "method", json::object(), json::object(), true, i);

    auto history = logger.GetHistory(3, 0, "", "");
    EXPECT_EQ(history.size(), 3);
    // Most recent entries
    EXPECT_EQ(history[0].duration_us, 9);
    EXPECT_EQ(history[1].duration_us, 8);
    EXPECT_EQ(history[2].duration_us, 7);
}

TEST_F(CallLoggerTest, GetHistoryRespectsOffset) {
    for (int i = 0; i < 5; ++i)
        logger.Log("user", "svc", "method", json::object(), json::object(), true, i);

    auto history = logger.GetHistory(50, 2, "", "");
    EXPECT_EQ(history.size(), 3);
    EXPECT_EQ(history[0].duration_us, 2);
    EXPECT_EQ(history[1].duration_us, 1);
    EXPECT_EQ(history[2].duration_us, 0);
}

TEST_F(CallLoggerTest, GetHistoryOffsetExceedsSize) {
    logger.Log("user", "svc", "method", json::object(), json::object(), true, 0);
    auto history = logger.GetHistory(50, 100, "", "");
    EXPECT_EQ(history.size(), 0);
}

TEST_F(CallLoggerTest, GetHistoryFilterByService) {
    logger.Log("alice", "SheetService", "List", json::object(), json::object(), true, 10);
    logger.Log("bob", "FileService", "Upload", json::object(), json::object(), true, 20);
    logger.Log("carol", "SheetService", "Create", json::object(), json::object(), true, 30);

    auto sheet_only = logger.GetHistory(50, 0, "SheetService", "");
    EXPECT_EQ(sheet_only.size(), 2);
    EXPECT_EQ(sheet_only[0].service, "SheetService");
    EXPECT_EQ(sheet_only[1].service, "SheetService");
}

TEST_F(CallLoggerTest, GetHistoryFilterByMethod) {
    logger.Log("alice", "SheetService", "List", json::object(), json::object(), true, 10);
    logger.Log("bob", "SheetService", "Create", json::object(), json::object(), true, 20);
    logger.Log("carol", "SheetService", "List", json::object(), json::object(), true, 30);

    auto list_only = logger.GetHistory(50, 0, "", "List");
    EXPECT_EQ(list_only.size(), 2);
}

TEST_F(CallLoggerTest, GetHistoryFilterByServiceAndMethod) {
    logger.Log("alice", "SheetService", "List", json::object(), json::object(), true, 10);
    logger.Log("bob", "FileService", "List", json::object(), json::object(), true, 20);
    logger.Log("carol", "SheetService", "Create", json::object(), json::object(), true, 30);

    auto filtered = logger.GetHistory(50, 0, "SheetService", "List");
    EXPECT_EQ(filtered.size(), 1);
    EXPECT_EQ(filtered[0].username, "alice");
}

TEST_F(CallLoggerTest, TotalCountTracksEntries) {
    EXPECT_EQ(logger.TotalCount(), 0);
    logger.Log("u", "s", "m", json::object(), json::object(), true, 0);
    EXPECT_EQ(logger.TotalCount(), 1);
    logger.Log("u2", "s2", "m2", json::object(), json::object(), false, 0);
    EXPECT_EQ(logger.TotalCount(), 2);
}

TEST_F(CallLoggerTest, QueueOverflowEvictsOldest) {
    CallLogger small_logger(3, nullptr);  // max 3 entries

    small_logger.Log("a", "s", "m", json::object(), json::object(), true, 1);
    small_logger.Log("b", "s", "m", json::object(), json::object(), true, 2);
    small_logger.Log("c", "s", "m", json::object(), json::object(), true, 3);
    small_logger.Log("d", "s", "m", json::object(), json::object(), true, 4);

    EXPECT_EQ(small_logger.TotalCount(), 3);

    auto history = small_logger.GetHistory(50, 0, "", "");
    ASSERT_EQ(history.size(), 3);
    // "a" should be evicted; most recent are d, c, b
    EXPECT_EQ(history[0].username, "d");
    EXPECT_EQ(history[1].username, "c");
    EXPECT_EQ(history[2].username, "b");
}

TEST_F(CallLoggerTest, LogCapturesTimestamp) {
    logger.Log("test", "svc", "method", json::object(), json::object(), true, 0);
    auto history = logger.GetHistory(1, 0, "", "");
    ASSERT_EQ(history.size(), 1);
    // Timestamp format: "YYYY-MM-DD HH:MM:SS.mmm"
    EXPECT_EQ(history[0].timestamp.size(), 23);
    EXPECT_EQ(history[0].timestamp[4], '-');
    EXPECT_EQ(history[0].timestamp[7], '-');
    EXPECT_EQ(history[0].timestamp[10], ' ');
    EXPECT_EQ(history[0].timestamp[13], ':');
}

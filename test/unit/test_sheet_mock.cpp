#include <gtest/gtest.h>

#include "mocks.h"
#include "spreadsheet_service_impl.h"

using ::testing::_;
using ::testing::DoAll;
using ::testing::Return;
using ::testing::SetArgReferee;

// ===== GetSpreadsheet — Redis cache hit =====
TEST(SheetMock, GetSpreadsheetRedisCacheHit) {
    MockDB db;
    MockRedis redis;
    SpreadsheetServiceImpl svc;

    svc.SetDatabase(&db);
    svc.SetRedis(&redis);

    // Ownership check: caller 42 owns sheet 1
    EXPECT_CALL(db, GetSpreadsheetOwner(1, _, _))
        .WillOnce(DoAll(SetArgReferee<1>(42), Return(true)));

    EXPECT_CALL(redis, IsConnected()).WillRepeatedly(Return(true));

    // Redis returns a cached GetSpreadsheetResponse
    rpc::GetSpreadsheetResponse cached_resp;
    cached_resp.set_success(true);
    auto *s = cached_resp.mutable_spreadsheet();
    s->set_id(1);
    s->set_name("cached-sheet");
    std::string ser;
    ASSERT_TRUE(cached_resp.SerializeToString(&ser));

    // GetJSON returns the cached proto (ts key and data key both hit)
    EXPECT_CALL(redis, GetJSON(_, _))
        .WillRepeatedly(DoAll(SetArgReferee<1>(ser), Return(true)));

    // Allow other redis calls (lock, etc.)
    EXPECT_CALL(redis, SetNX(_, _, _)).WillRepeatedly(Return(true));
    EXPECT_CALL(redis, SetJSON(_, _, _)).WillRepeatedly(Return(true));
    EXPECT_CALL(redis, DeleteKey(_)).WillRepeatedly(Return(true));

    // DB must NOT be touched on cache hit
    EXPECT_CALL(db, GetSpreadsheet(_, _, _)).Times(0);

    grpc::ServerContext ctx;
    rpc::GetSpreadsheetRequest req;
    rpc::GetSpreadsheetResponse resp;
    req.set_id(1);
    req.set_user_id(42);

    auto status = svc.GetSpreadsheet(&ctx, &req, &resp);
    EXPECT_TRUE(status.ok());
    EXPECT_TRUE(resp.success());
    EXPECT_EQ(resp.spreadsheet().id(), 1);
    EXPECT_EQ(resp.spreadsheet().name(), "cached-sheet");
}

// ===== GetSpreadsheet — cache miss, DB fallback =====
TEST(SheetMock, GetSpreadsheetCacheMissThenDbHit) {
    MockDB db;
    MockRedis redis;
    SpreadsheetServiceImpl svc;

    svc.SetDatabase(&db);
    svc.SetRedis(&redis);

    // Ownership check
    EXPECT_CALL(db, GetSpreadsheetOwner(2, _, _))
        .WillOnce(DoAll(SetArgReferee<1>(42), Return(true)));

    EXPECT_CALL(redis, IsConnected()).WillRepeatedly(Return(true));

    // Redis: all keys miss
    EXPECT_CALL(redis, GetJSON(_, _)).WillRepeatedly(Return(false));

    // Redis: allow SetNX/SetJSON/DeleteKey for logical-TTL
    EXPECT_CALL(redis, SetNX(_, _, _)).WillRepeatedly(Return(true));
    EXPECT_CALL(redis, SetJSON(_, _, _)).WillRepeatedly(Return(true));
    EXPECT_CALL(redis, DeleteKey(_)).WillRepeatedly(Return(true));
    EXPECT_CALL(redis, ExpireKey(_, _)).WillRepeatedly(Return(true));

    // DB: hit
    SpreadsheetRow row;
    row.id = 2;
    row.name = "db-sheet";
    row.description = "from-db";
    EXPECT_CALL(db, GetSpreadsheet(2, 42, _))
        .WillOnce(DoAll(SetArgReferee<2>(row), Return(true)));

    grpc::ServerContext ctx;
    rpc::GetSpreadsheetRequest req;
    rpc::GetSpreadsheetResponse resp;
    req.set_id(2);
    req.set_user_id(42);

    auto status = svc.GetSpreadsheet(&ctx, &req, &resp);
    EXPECT_TRUE(status.ok());
    EXPECT_TRUE(resp.success());
}

// ===== GetSpreadsheet — wrong owner =====
TEST(SheetMock, GetSpreadsheetWrongOwner) {
    MockDB db;
    SpreadsheetServiceImpl svc;
    svc.SetDatabase(&db);

    EXPECT_CALL(db, GetSpreadsheetOwner(3, _, _))
        .WillOnce(DoAll(SetArgReferee<1>(99), Return(true)));

    grpc::ServerContext ctx;
    rpc::GetSpreadsheetRequest req;
    rpc::GetSpreadsheetResponse resp;
    req.set_id(3);
    req.set_user_id(42);

    auto status = svc.GetSpreadsheet(&ctx, &req, &resp);
    EXPECT_TRUE(status.ok());
    EXPECT_FALSE(resp.success());
}

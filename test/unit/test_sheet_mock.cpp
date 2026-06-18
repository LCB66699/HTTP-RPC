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
    // auth_ and auth_stub_ both nullptr → auth check skipped

    // Ownership check
    EXPECT_CALL(db, GetSpreadsheetOwner(1, _, _))
        .WillOnce(DoAll(SetArgReferee<1>(42), Return(true)));

    // Redis: connected
    EXPECT_CALL(redis, IsConnected()).WillRepeatedly(Return(true));

    // Redis: cache hit — return serialized SpreadsheetRow
    rpc::SpreadsheetRow cached;
    cached.set_id(1);
    cached.set_name("cached-sheet");
    cached.set_description("from-redis");
    std::string ser;
    ASSERT_TRUE(cached.SerializeToString(&ser));
    EXPECT_CALL(redis, GetJSON("sheet:1", _))
        .WillOnce(DoAll(SetArgReferee<1>(ser), Return(true)));

    // DB must NOT be touched on cache hit
    EXPECT_CALL(db, GetSpreadsheet(_, _, _)).Times(0);

    grpc::ServerContext ctx;
    rpc::GetSpreadsheetRequest req;
    rpc::GetSpreadsheetResponse resp;
    req.set_id(1);
    req.set_user_id(42);

    auto status = svc.GetSpreadsheet(&ctx, &req, &resp);
    EXPECT_TRUE(status.ok());
    EXPECT_EQ(resp.id(), 1);
    EXPECT_EQ(resp.name(), "cached-sheet");
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

    // Redis: both data and timestamp keys miss
    EXPECT_CALL(redis, GetJSON("sheet:2", _)).WillOnce(Return(false));
    EXPECT_CALL(redis, GetJSON("sheet:2:ts", _)).WillOnce(Return(false));

    // DB: hit
    SpreadsheetRow row;
    row.set_id(2);
    row.set_name("db-sheet");
    row.set_owner_uid(42);
    EXPECT_CALL(db, GetSpreadsheet(2, 42, _))
        .WillOnce(DoAll(SetArgReferee<2>(row), Return(true)));

    // Redis: write-back cache (data + ts keys)
    EXPECT_CALL(redis, SetJSON(_, _, _)).Times(2);

    grpc::ServerContext ctx;
    rpc::GetSpreadsheetRequest req;
    rpc::GetSpreadsheetResponse resp;
    req.set_id(2);
    req.set_user_id(42);

    auto status = svc.GetSpreadsheet(&ctx, &req, &resp);
    EXPECT_TRUE(status.ok());
    EXPECT_EQ(resp.id(), 2);
}

// ===== GetSpreadsheet — wrong owner =====
TEST(SheetMock, GetSpreadsheetWrongOwner) {
    MockDB db;
    SpreadsheetServiceImpl svc;
    svc.SetDatabase(&db);

    // Owner is 99, but caller is 42
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

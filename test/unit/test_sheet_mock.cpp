#include <gtest/gtest.h>

#include <thread>

#include "mocks.h"
#include "shared/base/rpc_interceptor.h"
#include "sheet/spreadsheet_service_impl.h"

using ::testing::_;
using ::testing::DoAll;
using ::testing::Return;
using ::testing::SetArgReferee;

// RAII guard: set g_rpc_auth_ctx, restore on teardown
class AuthGuard {
public:
    AuthGuard(int64_t uid, const std::string &user = "test") {
        saved_ = g_rpc_auth_ctx;
        g_rpc_auth_ctx.authenticated = true;
        g_rpc_auth_ctx.user_id = uid;
        g_rpc_auth_ctx.username = user;
    }
    ~AuthGuard() { g_rpc_auth_ctx = saved_; }
private:
    AuthContext saved_;
};

// ===== GetSpreadsheet �?Redis cache hit =====
TEST(SheetMock, GetSpreadsheetRedisCacheHit) {
    MockDB db;
    MockRedis redis;
    SpreadsheetServiceImpl svc;

    svc.SetDatabase(&db);
    svc.SetRedis(&redis);

    AuthGuard auth(42);

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

    // GetJSON returns the cached proto for data key, a valid timestamp for ts key.
    // Without this, the stale-refresh check (stoll on ts) would throw and spawn a
    // detached thread that outlives the test, causing use-after-free in the next test.
    EXPECT_CALL(redis, GetJSON(_, _))
        .WillRepeatedly([&ser](const std::string &key, std::string &value) -> bool {
            if (key.find(":ts") != std::string::npos) {
                value = std::to_string(std::time(nullptr));
            } else {
                value = ser;
            }
            return true;
        });

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

// ===== GetSpreadsheet �?cache miss, DB fallback =====
TEST(SheetMock, GetSpreadsheetCacheMissThenDbHit) {
    MockDB db;
    MockRedis redis;
    SpreadsheetServiceImpl svc;

    svc.SetDatabase(&db);
    svc.SetRedis(&redis);

    AuthGuard auth(42);

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

    // 等后台异步刷新线程跑完，避免析构时野指针
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

// ===== GetSpreadsheet �?wrong owner =====
TEST(SheetMock, GetSpreadsheetWrongOwner) {
    MockDB db;
    SpreadsheetServiceImpl svc;
    svc.SetDatabase(&db);

    AuthGuard auth(42);

    EXPECT_CALL(db, GetSpreadsheetOwner(3, _, _))
        .WillOnce(DoAll(SetArgReferee<1>(99), Return(true)));

    grpc::ServerContext ctx;
    rpc::GetSpreadsheetRequest req;
    rpc::GetSpreadsheetResponse resp;
    req.set_id(3);
    req.set_user_id(42);

    auto status = svc.GetSpreadsheet(&ctx, &req, &resp);
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.error_code(), grpc::NOT_FOUND);
}

// ===== GetSpreadsheet — DB hit + Mongo fills cells =====
TEST(SheetMock, GetSpreadsheetMongoFillsCells) {
    AuthGuard auth(42);
    MockDB db;
    MockRedis redis;
    MockMongo mongo;
    SpreadsheetServiceImpl svc;

    svc.SetDatabase(&db);
    svc.SetRedis(&redis);
    svc.SetMongo(&mongo);

    EXPECT_CALL(db, GetSpreadsheetOwner(1, _, _))
        .WillOnce(DoAll(SetArgReferee<1>(42), Return(true)));

    EXPECT_CALL(redis, IsConnected()).WillRepeatedly(Return(true));
    EXPECT_CALL(redis, GetJSON(_, _)).WillRepeatedly(Return(false));
    EXPECT_CALL(redis, SetNX(_, _, _)).WillRepeatedly(Return(true));
    EXPECT_CALL(redis, SetJSON(_, _, _)).WillRepeatedly(Return(true));
    EXPECT_CALL(redis, DeleteKey(_)).WillRepeatedly(Return(true));
    EXPECT_CALL(redis, ExpireKey(_, _)).WillRepeatedly(Return(true));

    SpreadsheetRow row;
    row.id = 1;
    row.name = "mongo-sheet";
    EXPECT_CALL(db, GetSpreadsheet(1, 42, _))
        .WillOnce(DoAll(SetArgReferee<2>(row), Return(true)));

    // Mongo returns cells
    EXPECT_CALL(mongo, GetSheetCells(1, _, _))
        .WillOnce(DoAll(SetArgReferee<1>("[\"A\",\"B\"]"),
                        SetArgReferee<2>("[[\"x\",\"y\"]]"),
                        Return(true)));

    grpc::ServerContext ctx;
    rpc::GetSpreadsheetRequest req;
    rpc::GetSpreadsheetResponse resp;
    req.set_id(1);
    req.set_user_id(42);

    auto status = svc.GetSpreadsheet(&ctx, &req, &resp);
    EXPECT_TRUE(status.ok());
    EXPECT_TRUE(resp.success());
    EXPECT_EQ(resp.spreadsheet().headers_json(), "[\"A\",\"B\"]");
    EXPECT_EQ(resp.spreadsheet().data_json(), "[[\"x\",\"y\"]]");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

// ===== GetSpreadsheet — Mongo miss, cells remain empty =====
TEST(SheetMock, GetSpreadsheetMongoMissFallback) {
    AuthGuard auth(42);
    MockDB db;
    MockRedis redis;
    MockMongo mongo;
    SpreadsheetServiceImpl svc;

    svc.SetDatabase(&db);
    svc.SetRedis(&redis);
    svc.SetMongo(&mongo);

    EXPECT_CALL(db, GetSpreadsheetOwner(2, _, _))
        .WillOnce(DoAll(SetArgReferee<1>(42), Return(true)));

    EXPECT_CALL(redis, IsConnected()).WillRepeatedly(Return(true));
    EXPECT_CALL(redis, GetJSON(_, _)).WillRepeatedly(Return(false));
    EXPECT_CALL(redis, SetNX(_, _, _)).WillRepeatedly(Return(true));
    EXPECT_CALL(redis, SetJSON(_, _, _)).WillRepeatedly(Return(true));
    EXPECT_CALL(redis, DeleteKey(_)).WillRepeatedly(Return(true));
    EXPECT_CALL(redis, ExpireKey(_, _)).WillRepeatedly(Return(true));

    SpreadsheetRow row;
    row.id = 2;
    row.name = "no-mongo";
    EXPECT_CALL(db, GetSpreadsheet(2, 42, _))
        .WillOnce(DoAll(SetArgReferee<2>(row), Return(true)));

    // Mongo returns false — cells stay empty
    EXPECT_CALL(mongo, GetSheetCells(2, _, _))
        .WillOnce(Return(false));

    grpc::ServerContext ctx;
    rpc::GetSpreadsheetRequest req;
    rpc::GetSpreadsheetResponse resp;
    req.set_id(2);
    req.set_user_id(42);

    auto status = svc.GetSpreadsheet(&ctx, &req, &resp);
    EXPECT_TRUE(status.ok());
    EXPECT_TRUE(resp.success());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

// ===== UpdateSpreadsheet — Mongo upsert on successful update =====
TEST(SheetMock, UpdateSpreadsheetMongoUpsert) {
    AuthGuard auth(42);
    MockDB db;
    MockRedis redis;
    MockMongo mongo;
    SpreadsheetServiceImpl svc;

    svc.SetDatabase(&db);
    svc.SetRedis(&redis);
    svc.SetMongo(&mongo);

    EXPECT_CALL(db, GetSpreadsheetOwner(3, _, _))
        .WillOnce(DoAll(SetArgReferee<1>(42), Return(true)));

    EXPECT_CALL(db, UpdateSpreadsheet(3, 42, "updated", "desc", "[\"H\"]", "[[\"X\"]]", 0))
        .WillOnce(Return(true));

    EXPECT_CALL(mongo, UpsertSheetCells(3, 42, "[\"H\"]", "[[\"X\"]]"))
        .WillOnce(Return(true));

    EXPECT_CALL(redis, IsConnected()).WillRepeatedly(Return(true));
    EXPECT_CALL(redis, DeleteKey(_)).WillRepeatedly(Return(true));
    EXPECT_CALL(redis, Increment(_)).WillRepeatedly(Return(1));
    EXPECT_CALL(redis, Publish(_, _)).WillRepeatedly(Return(true));

    grpc::ServerContext ctx;
    rpc::UpdateSpreadsheetRequest req;
    rpc::UpdateSpreadsheetResponse resp;
    req.set_id(3);
    req.set_name("updated");
    req.set_description("desc");
    req.set_headers_json("[\"H\"]");
    req.set_data_json("[[\"X\"]]");

    auto status = svc.UpdateSpreadsheet(&ctx, &req, &resp);
    EXPECT_TRUE(status.ok());
    EXPECT_TRUE(resp.success());
}

// ===== UpdateSpreadsheet — Mongo fail does not rollback DB (best-effort) =====
TEST(SheetMock, UpdateSpreadsheetMongoFailBestEffort) {
    AuthGuard auth(42);
    MockDB db;
    MockRedis redis;
    MockMongo mongo;
    SpreadsheetServiceImpl svc;

    svc.SetDatabase(&db);
    svc.SetRedis(&redis);
    svc.SetMongo(&mongo);

    EXPECT_CALL(db, GetSpreadsheetOwner(4, _, _))
        .WillOnce(DoAll(SetArgReferee<1>(42), Return(true)));

    EXPECT_CALL(db, UpdateSpreadsheet(4, 42, "ok", "desc", "[]", "[]", 0))
        .WillOnce(Return(true));

    // Mongo fails — but DB already committed, no rollback
    EXPECT_CALL(mongo, UpsertSheetCells(4, 42, "[]", "[]"))
        .WillOnce(Return(false));

    EXPECT_CALL(redis, IsConnected()).WillRepeatedly(Return(true));
    EXPECT_CALL(redis, DeleteKey(_)).WillRepeatedly(Return(true));
    EXPECT_CALL(redis, Increment(_)).WillRepeatedly(Return(1));
    EXPECT_CALL(redis, Publish(_, _)).WillRepeatedly(Return(true));

    grpc::ServerContext ctx;
    rpc::UpdateSpreadsheetRequest req;
    rpc::UpdateSpreadsheetResponse resp;
    req.set_id(4);
    req.set_name("ok");
    req.set_description("desc");
    req.set_headers_json("[]");
    req.set_data_json("[]");

    auto status = svc.UpdateSpreadsheet(&ctx, &req, &resp);
    EXPECT_TRUE(status.ok());
    EXPECT_TRUE(resp.success());  // DB wrote, Mongo is best-effort
}

// ===== DeleteSpreadsheet — Mongo delete on success =====
TEST(SheetMock, DeleteSpreadsheetMongoDelete) {
    AuthGuard auth(42);
    MockDB db;
    MockRedis redis;
    MockMongo mongo;
    SpreadsheetServiceImpl svc;

    svc.SetDatabase(&db);
    svc.SetRedis(&redis);
    svc.SetMongo(&mongo);

    EXPECT_CALL(db, GetSpreadsheetOwner(5, _, _))
        .WillOnce(DoAll(SetArgReferee<1>(42), Return(true)));

    EXPECT_CALL(mongo, DeleteSheetCells(5))
        .WillOnce(Return(true));

    EXPECT_CALL(db, DeleteSpreadsheet(5, 42))
        .WillOnce(Return(true));

    EXPECT_CALL(redis, IsConnected()).WillRepeatedly(Return(true));
    EXPECT_CALL(redis, DeleteKey(_)).WillRepeatedly(Return(true));
    EXPECT_CALL(redis, Increment(_)).WillRepeatedly(Return(1));
    EXPECT_CALL(redis, Publish(_, _)).WillRepeatedly(Return(true));

    grpc::ServerContext ctx;
    rpc::DeleteSpreadsheetRequest req;
    rpc::DeleteSpreadsheetResponse resp;
    req.set_id(5);

    auto status = svc.DeleteSpreadsheet(&ctx, &req, &resp);
    EXPECT_TRUE(status.ok());
    EXPECT_TRUE(resp.success());
}

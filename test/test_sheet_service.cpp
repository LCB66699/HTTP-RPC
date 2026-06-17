#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <grpcpp/grpcpp.h>

#include "../server/include/spreadsheet_service_impl.h"
#include "../server/include/auth_interceptor.h"
#include "../server/include/call_logger.h"
#include "../server/include/l1_cache.h"
#include "../server/include/redis_client.h"
#include "../server/include/rabbit_publisher.h"
#include "../server/include/sheet_helpers.h"
#include "../server/include/system_logger.h"

using ::testing::_;
using ::testing::DoAll;
using ::testing::Return;
using ::testing::SetArgReferee;

// ===== Mock Database =====
class MockDatabase : public IDatabase {
   public:
    MOCK_METHOD(bool, CreateSpreadsheet,
                (int64_t user_id, const std::string &username, const std::string &name, const std::string &desc,
                 const std::string &headers_json, const std::string &data_json, int64_t &out_id,
                 const std::string &idempotency_key),
                (override));
    MOCK_METHOD(bool, GetSpreadsheet, (int64_t id, int64_t user_id, SpreadsheetRow &out), (override));
    MOCK_METHOD(bool, ListSpreadsheets,
                (int64_t user_id, std::vector<SpreadsheetSummary> &out, int &total, int page, int page_size),
                (override));
    MOCK_METHOD(bool, UpdateSpreadsheet,
                (int64_t id, int64_t user_id, const std::string &name, const std::string &desc,
                 const std::string &headers_json, const std::string &data_json, int version),
                (override));
    MOCK_METHOD(bool, DeleteSpreadsheet, (int64_t id, int64_t user_id), (override));
    MOCK_METHOD(bool, GetSpreadsheetOwner, (int64_t id, int64_t &owner_user_id, int *out_version), (override));
    MOCK_METHOD(bool, UpdateSpreadsheetStoragePath, (int64_t id, const std::string &storage_path), (override));
    MOCK_METHOD(bool, InsertOutbox, (int64_t user_id, const std::string &event_type, const std::string &payload,
                                      const std::string &trace_context),
                (override));

    MOCK_METHOD(bool, CreateFile,
                (int64_t user_id, const std::string &username, const std::string &original_name, int64_t size,
                 const std::string &mime_type, const std::string &storage_key, int64_t &out_id,
                 const std::string &idempotency_key),
                (override));
    MOCK_METHOD(bool, GetFile, (int64_t id, int64_t user_id, FileRow &out), (override));
    MOCK_METHOD(bool, ListFiles, (int64_t user_id, std::vector<FileRow> &out, int &total, int page, int page_size),
                (override));
    MOCK_METHOD(bool, DeleteFile, (int64_t id, int64_t user_id), (override));
    MOCK_METHOD(bool, GetFileOwner, (int64_t id, int64_t &owner_user_id), (override));
    MOCK_METHOD(bool, UpdateFileContent, (int64_t id, const std::string &content), (override));
};

// ===== Unit Tests =====

TEST(SheetService, CreateSpreadsheetSucceeds) {
    MockDatabase db;
    SpreadsheetServiceImpl svc;
    svc.SetDatabase(&db);

    EXPECT_CALL(db, CreateSpreadsheet(42, "tester", "MySheet", "desc", "[]", "[]", _, _))
        .WillOnce(DoAll(SetArgReferee<6>(12345), Return(true)));

    rpc::CreateSpreadsheetRequest req;
    req.set_user_id(42);
    req.set_name("MySheet");
    req.set_description("desc");
    req.set_headers_json("[]");
    req.set_data_json("[]");

    rpc::CreateSpreadsheetResponse resp;
    // CreateSpreadsheet 内部会调用 auth_->Authenticate，但 auth_ 是 nullptr
    // 此时 Authenticate 不会被调用（null check）
    // 然后调用 db_->CreateSpreadsheet，会被 mock 拦截
    // 但日志和 Redis 调用会访问 nullptr...
    //
    // 实际测试需要完整的 DI 框架 — 这是概念验证
    GTEST_SKIP() << "Full DI refactoring needed for complete handler test";
}

TEST(SheetService, CreateSpreadsheetEmptyName) {
    // 测试输入验证：空名称
    SpreadsheetServiceImpl svc;

    rpc::CreateSpreadsheetRequest req;
    req.set_name("");  // 空名称
    rpc::CreateSpreadsheetResponse resp;

    // 不带 db_ 时调用应设置错误
    // grpc::Status st = svc.CreateSpreadsheet(nullptr, &req, &resp);
    // EXPECT_FALSE(resp.success());
    // EXPECT_EQ(resp.error(), "Name is required");
    GTEST_SKIP() << "Requires gRPC ServerContext mock";
}

// Cache key helpers — pure functions, no mocks needed
TEST(SheetService, CacheKeyFormats) {
    EXPECT_EQ(SheetVersionKey(10), "u:10:sheets:version");
    EXPECT_EQ(SheetCacheKey(7, 42), "u:7:sheet:42");
    EXPECT_EQ(SheetLockKey(7, 42), "lock:u:7:sheet:42");

    // Pagination test
    auto key = SheetListCacheKey(5, 2, 0, 0);
    EXPECT_EQ(key, "u:5:sheets:v2");

    key = SheetListCacheKey(5, 2, 1, 20);
    EXPECT_EQ(key, "u:5:sheets:v2:p1:ps20");
}

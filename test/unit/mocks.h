#pragma once
#include <gmock/gmock.h>

#include <vector>

#include "shared/client/database.h"
#include "shared/base/service_interfaces.h"
#include "shared/client/mongo_client.h"

class MockDB : public IDatabase {
public:
    // ---- Spreadsheet ----
    MOCK_METHOD(bool, CreateSpreadsheet,
        (int64_t uid, const std::string &uname, const std::string &name,
         const std::string &desc, const std::string &headers, const std::string &data,
         int64_t &out_id, const std::string &idempotency_key), (override));
    MOCK_METHOD(bool, GetSpreadsheet, (int64_t id, int64_t uid, SpreadsheetRow &out), (override));
    MOCK_METHOD(bool, ListSpreadsheets,
        (int64_t uid, std::vector<SpreadsheetSummary> &out,
         std::string &next_cursor, bool &has_more,
         int limit, const std::string &cursor), (override));
    MOCK_METHOD(bool, UpdateSpreadsheet,
        (int64_t id, int64_t uid, const std::string &name, const std::string &desc,
         const std::string &headers, const std::string &data, int version), (override));
    MOCK_METHOD(bool, DeleteSpreadsheet, (int64_t id, int64_t uid), (override));
    MOCK_METHOD(bool, GetSpreadsheetOwner,
        (int64_t id, int64_t &owner_uid, int *out_version), (override));
    MOCK_METHOD(bool, UpdateSpreadsheetStoragePath,
        (int64_t id, const std::string &storage_path), (override));

    // ---- File ----
    MOCK_METHOD(bool, CreateFile,
        (int64_t uid, const std::string &uname, const std::string &original_name,
         int64_t size, const std::string &mime, const std::string &storage_key,
         int64_t &out_id, const std::string &idempotency_key), (override));
    MOCK_METHOD(bool, GetFile, (int64_t id, int64_t uid, FileRow &out), (override));
    MOCK_METHOD(bool, ListFiles,
        (int64_t uid, std::vector<FileRow> &out,
         std::string &next_cursor, bool &has_more,
         int limit, const std::string &cursor), (override));
    MOCK_METHOD(bool, DeleteFile, (int64_t id, int64_t uid), (override));
    MOCK_METHOD(bool, GetFileOwner, (int64_t id, int64_t &owner_uid), (override));
    MOCK_METHOD(bool, UpdateFileContent, (int64_t id, const std::string &content, int version), (override));
    MOCK_METHOD(bool, CreateFolder,
        (int64_t uid, const std::string &name, int64_t parent_id, int64_t &out_id), (override));
    MOCK_METHOD(bool, MoveFile, (int64_t id, int64_t target_folder_id, int version), (override));
    MOCK_METHOD(int, BatchDeleteFiles, (int64_t uid, const std::vector<int64_t> &ids), (override));

    // ---- Outbox ----
    MOCK_METHOD(bool, InsertOutbox,
        (int64_t uid, const std::string &event_type, const std::string &payload,
         const std::string &trace_context), (override));
};

class MockRedis : public IRedisClient {
public:
    MOCK_METHOD(bool, IsConnected, (), (const, override));
    MOCK_METHOD(int64_t, Increment, (const std::string &key), (override));
    MOCK_METHOD(bool, GetJSON, (const std::string &key, std::string &value), (override));
    MOCK_METHOD(bool, SetJSON, (const std::string &key, const std::string &value, int ttl), (override));
    MOCK_METHOD(bool, SetNX, (const std::string &key, const std::string &value, int ttl), (override));
    MOCK_METHOD(bool, DeleteKey, (const std::string &key), (override));
    MOCK_METHOD(bool, ExpireKey, (const std::string &key, int ttl), (override));
    MOCK_METHOD(int64_t, GetInt, (const std::string &key), (override));
    MOCK_METHOD(bool, Publish, (const std::string &channel, const std::string &message), (override));
};

class MockRabbit : public IRabbitPublisher {
public:
    MOCK_METHOD(bool, Publish,
        (const std::string &exchange, const std::string &routing_key, const std::string &body),
        (override));
};

class MockMongo : public IMongoClient {
public:
    MOCK_METHOD(bool, UpsertSheetCells,
        (int64_t sheet_id, int64_t user_id, const std::string &headers_json,
         const std::string &data_json), (override));
    MOCK_METHOD(bool, GetSheetCells,
        (int64_t sheet_id, std::string &headers_json, std::string &data_json), (override));
    MOCK_METHOD(bool, DeleteSheetCells, (int64_t sheet_id), (override));
};

#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct SpreadsheetRow;
struct SpreadsheetSummary;
struct FileRow;

// Abstract interfaces for dependency injection in service tests.
// Concrete classes (ShardedDatabase, RedisClient, etc.) implicitly satisfy these.

class IDatabase {
   public:
    virtual ~IDatabase() = default;
    virtual bool CreateSpreadsheet(int64_t user_id, const std::string &username, const std::string &name,
                                   const std::string &desc, const std::string &headers_json,
                                   const std::string &data_json, int64_t &out_id,
                                   const std::string &idempotency_key = "") = 0;
    virtual bool GetSpreadsheet(int64_t id, int64_t user_id, SpreadsheetRow &out) = 0;
    virtual bool ListSpreadsheets(int64_t user_id, std::vector<SpreadsheetSummary> &out, int &total, int page = 0,
                                  int page_size = 0, int64_t after_id = 0) = 0;
    virtual bool UpdateSpreadsheet(int64_t id, int64_t user_id, const std::string &name, const std::string &desc,
                                   const std::string &headers_json, const std::string &data_json, int version = 0) = 0;
    virtual bool DeleteSpreadsheet(int64_t id, int64_t user_id = 0) = 0;
    virtual bool GetSpreadsheetOwner(int64_t id, int64_t &owner_user_id, int *out_version = nullptr) = 0;
    virtual bool UpdateSpreadsheetStoragePath(int64_t id, const std::string &storage_path) = 0;
    virtual bool InsertOutbox(int64_t user_id, const std::string &event_type, const std::string &payload,
                              const std::string &trace_context = "") = 0;

    virtual bool CreateFile(int64_t user_id, const std::string &username, const std::string &original_name, int64_t size,
                            const std::string &mime_type, const std::string &storage_key, int64_t &out_id,
                            const std::string &idempotency_key = "") = 0;
    virtual bool GetFile(int64_t id, int64_t user_id, FileRow &out) = 0;
    virtual bool ListFiles(int64_t user_id, std::vector<FileRow> &out, int &total, int page = 0, int page_size = 0,
                           int64_t after_id = 0) = 0;
    virtual bool DeleteFile(int64_t id, int64_t user_id = 0) = 0;
    virtual bool GetFileOwner(int64_t id, int64_t &owner_user_id) = 0;
    virtual bool UpdateFileContent(int64_t id, const std::string &content) = 0;
};

class IRedisClient {
   public:
    virtual ~IRedisClient() = default;
    virtual bool IsConnected() = 0;
    virtual bool Increment(const std::string &key) = 0;
    virtual bool GetJSON(const std::string &key, std::string &value) = 0;
    virtual bool SetJSON(const std::string &key, const std::string &value, int ttl) = 0;
    virtual bool SetNX(const std::string &key, const std::string &value, int ttl) = 0;
    virtual bool DeleteKey(const std::string &key) = 0;
    virtual bool ExpireKey(const std::string &key, int ttl) = 0;
    virtual int64_t GetInt(const std::string &key) = 0;
};

class IRabbitPublisher {
   public:
    virtual ~IRabbitPublisher() = default;
    virtual bool Publish(const std::string &exchange, const std::string &routing_key, const std::string &body) = 0;
};

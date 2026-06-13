// MySQL 读写分离层: 主库写(连接池) + 从库读(连接池 round-robin)
// libmysqlclient C API via TCP 3306
#pragma once
#include <mysql/mysql.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class Snowflake;

struct SpreadsheetRow {
    int64_t id = 0;
    std::string username, name, description, headers_json, data_json;
    int row_count = 0, col_count = 0;
    std::string created_at, updated_at;
    int version = 1;
    std::string storage_path;
};

struct SpreadsheetSummary {
    int64_t id = 0;
    std::string name, description;
    int row_count = 0, col_count = 0;
    std::string updated_at;
};

struct FileRow {
    int64_t id = 0;
    std::string username, original_name;
    int64_t size = 0;
    std::string mime_type, created_at, file_content;
    std::string storage_path;  // non-empty → content lives in object storage, not
                               // file_content
};

class Database {
   public:
    // write_host: MySQL master
    // read_hosts: 从库地址, 逗号分隔
    // write_pool_size: 写连接池大小, 默认4; 设为1即退化为原单连接模式
    Database(const std::string &write_host, int write_port, const std::string &read_hosts, int read_port,
             const std::string &user, const std::string &password, const std::string &db_name, int write_pool_size = 4);
    ~Database();

    bool Initialize();

    void SetSnowflake(Snowflake *sf) { snowflake_ = sf; }

    // Users
    bool AddUser(const std::string &username, const std::string &password_hash);
    bool GetUser(const std::string &username, std::string &password_hash_out);
    bool UserExists(const std::string &username);
    bool ImportFromUsersJson(const std::string &json_path);
    int GetTokenVersion(const std::string &username);
    bool IncrementTokenVersion(const std::string &username);
    // Returns users.id for embedding in JWT; -1 on failure.
    int64_t GetUserId(const std::string &username);

    // Spreadsheets — caller passes user_id (users.id) as the owner key.
    // username is stored alongside for display; idempotency_key deduplicates
    // retries.
    bool CreateSpreadsheet(int64_t user_id, const std::string &username, const std::string &name,
                           const std::string &desc, const std::string &headers_json, const std::string &data_json,
                           int64_t &out_id, const std::string &idempotency_key = "");
    bool GetSpreadsheet(int64_t id, int64_t user_id, SpreadsheetRow &out);
    // page is 0-based; page_size=0 disables pagination and returns all rows
    // (backward compat)
    bool ListSpreadsheets(int64_t user_id, std::vector<SpreadsheetSummary> &out, int &total, int page = 0,
                          int page_size = 0);
    bool UpdateSpreadsheet(int64_t id, int64_t user_id, const std::string &name, const std::string &desc,
                           const std::string &headers_json, const std::string &data_json, int version = 0);
    bool UpdateSpreadsheet(int64_t id, const std::string &name, const std::string &desc,
                           const std::string &headers_json, const std::string &data_json, int version = 0);
    bool DeleteSpreadsheet(int64_t id, int64_t user_id = 0);
    // Returns the owner's user_id and optionally the current version for
    // optimistic locking.
    bool GetSpreadsheetOwner(int64_t id, int64_t &owner_user_id, int *out_version = nullptr);

    // Files — same user_id convention.
    // storage_key: object-storage path returned by MinIO PutObject (empty =
    // legacy BLOB mode).
    bool CreateFile(int64_t user_id, const std::string &username, const std::string &original_name, int64_t size,
                    const std::string &mime_type, const std::string &storage_key, int64_t &out_id,
                    const std::string &idempotency_key = "");
    bool UpdateFileContent(int64_t id, const std::string &content);
    bool GetFile(int64_t id, int64_t user_id, FileRow &out);
    // page is 0-based; page_size=0 disables pagination and returns all rows
    // (backward compat)
    bool ListFiles(int64_t user_id, std::vector<FileRow> &out, int &total, int page = 0, int page_size = 0, int64_t after_id = 0);
    bool DeleteFile(int64_t id, int64_t user_id = 0);
    bool GetFileOwner(int64_t id, int64_t &owner_user_id);
    bool GetFileStoragePath(int64_t id, std::string &storage_path);
    bool GetSpreadsheetStoragePath(int64_t id, std::string &storage_path);
    bool UpdateSpreadsheetStoragePath(int64_t id, const std::string &storage_path);
    bool InsertOutbox(const std::string &event_type, const std::string &payload);

    // Undo log
    bool WriteUndoLog(const std::string &xid, const std::string &table_name, int64_t row_id,
                      const std::string &before_snapshot);
    bool GetUndoLog(const std::string &xid, std::string &table_name, int64_t &row_id, std::string &before_snapshot);
    bool ClearUndoLog(const std::string &xid);
    // Delete undo_log entries older than `days` days; call from a maintenance
    // goroutine/timer
    int PurgeOldUndoLogs(int days = 7);

    // Backward compat
    bool Exec(const std::string &sql) { return ExecWrite(sql); }
    MYSQL *GetConnection() { return write_conns_.empty() ? nullptr : write_conns_[0]->conn; }

    // 健康检查：后台线程每 30s PING 连接池，自动重建死连接
    void StartHealthCheck();
    void StopHealthCheck();

   private:
    std::string user_, password_, db_name_;
    std::string write_host_;  // 记录主库host供重连
    int write_port_, read_port_;
    int pool_size_;
    std::vector<std::string> read_hosts_;
    Snowflake *snowflake_ = nullptr;

    // 连接单元 — 读写连接池共用此结构
    struct PoolConn {
        MYSQL *conn = nullptr;
        std::mutex mtx;
    };

    // 写连接池（主库, 多连接并行写）
    std::vector<std::unique_ptr<PoolConn>> write_conns_;
    std::atomic<size_t> write_idx_{0};

    // 读连接池（从库, round-robin 无锁分发）
    std::vector<std::unique_ptr<PoolConn>> read_conns_;
    std::atomic<size_t> read_idx_{0};

    MYSQL *ConnectMYSQL(const std::string &host, int port);
    MYSQL *EscConn();  // 返回一个可用连接, 仅供 mysql_real_escape_string() 用
    bool ExecWrite(const std::string &sql);
    bool ExecWriteInsert(const std::string &sql, int64_t &out_id);
    MYSQL *GetReadConn();
    bool RunQuery(MYSQL *conn, const std::string &sql, MYSQL_RES **out_res);
    bool ExecRead(const std::string &sql, std::function<bool(MYSQL_RES *)> handler);

    std::thread health_check_;
    std::atomic<bool> health_running_{false};
    void HealthLoop();
};

// ---- ShardedDatabase: hash-routes by user_id across N Database shards ----
class ShardedDatabase {
   public:
    // shard_count: 分片数; host_prefix: "mysql-spreadsheet" →
    // "mysql-spreadsheet-0","mysql-spreadsheet-1"... db_name_prefix:
    // "rpc_spreadsheet" → "rpc_spreadsheet_0","rpc_spreadsheet_1"...
    ShardedDatabase(int shard_count, const std::string &write_host_prefix, int write_port,
                    const std::string &read_hosts_prefix, int read_port, const std::string &user,
                    const std::string &password, const std::string &db_name_prefix, int write_pool_size = 4);

    void InitializeAll();
    void SetSnowflake(Snowflake *sf) {
        for (auto &db : shards_)
            db->SetSnowflake(sf);
    }

    // === Users (auth — single shard) ===
    bool AddUser(const std::string &username, const std::string &password_hash);
    bool GetUser(const std::string &username, std::string &password_hash);
    bool UserExists(const std::string &username);
    int GetTokenVersion(const std::string &username);
    bool IncrementTokenVersion(const std::string &username);
    int64_t GetUserId(const std::string &username);
    bool ImportFromUsersJson(const std::string &json_path);

    // === Spreadsheets (hash by user_id) ===
    bool CreateSpreadsheet(int64_t user_id, const std::string &username, const std::string &name,
                           const std::string &desc, const std::string &headers_json, const std::string &data_json,
                           int64_t &out_id, const std::string &idempotency_key = "");
    bool GetSpreadsheet(int64_t id, int64_t user_id, SpreadsheetRow &out);
    bool ListSpreadsheets(int64_t user_id, std::vector<SpreadsheetSummary> &out, int &total, int page = 0,
                          int page_size = 0);
    bool UpdateSpreadsheet(int64_t id, int64_t user_id, const std::string &name, const std::string &desc,
                           const std::string &headers_json, const std::string &data_json, int version = 0);
    bool UpdateSpreadsheet(int64_t id, const std::string &name, const std::string &desc,
                           const std::string &headers_json, const std::string &data_json, int version = 0);
    bool DeleteSpreadsheet(int64_t id, int64_t user_id = 0);
    bool GetSpreadsheetOwner(int64_t id, int64_t &owner_user_id, int *out_version = nullptr);

    // === Files (hash by user_id) ===
    bool CreateFile(int64_t user_id, const std::string &username, const std::string &original_name, int64_t size,
                    const std::string &mime_type, const std::string &storage_key, int64_t &out_id,
                    const std::string &idempotency_key = "");
    bool UpdateFileContent(int64_t id, const std::string &content);
    bool GetFile(int64_t id, int64_t user_id, FileRow &out);
    bool ListFiles(int64_t user_id, std::vector<FileRow> &out, int &total, int page = 0, int page_size = 0, int64_t after_id = 0);
    bool DeleteFile(int64_t id, int64_t user_id = 0);
    bool GetFileOwner(int64_t id, int64_t &owner_user_id);
    bool GetFileStoragePath(int64_t id, std::string &storage_path);
    bool GetSpreadsheetStoragePath(int64_t id, std::string &storage_path);
    bool UpdateSpreadsheetStoragePath(int64_t id, const std::string &storage_path);
    bool InsertOutbox(int64_t user_id, const std::string &event_type, const std::string &payload);

    // === Undo Log (by user_id or broadcast) ===
    bool WriteUndoLog(const std::string &xid, const std::string &table_name, int64_t row_id,
                      const std::string &before_snapshot);
    bool GetUndoLog(const std::string &xid, std::string &table_name, int64_t &row_id, std::string &before_snapshot);
    bool ClearUndoLog(const std::string &xid);
    int PurgeOldUndoLogs(int days = 7);

    bool Exec(const std::string &sql) { return shards_[0]->Exec(sql); }
    MYSQL *GetConnection() { return shards_[0]->GetConnection(); }

    void StartHealthCheck() {
        for (auto &db : shards_)
            db->StartHealthCheck();
    }
    void StopHealthCheck() {
        for (auto &db : shards_)
            db->StopHealthCheck();
    }

    int ShardCount() const { return shard_count_; }
    Database *ShardFor(int64_t user_id);
    Database *ShardForBroadcast();  // 返回第一个分片（用于广播类查询）

    Database *AuthDB() { return shards_[0].get(); }

   private:
    int shard_count_;
    std::vector<std::unique_ptr<Database>> shards_;
    std::string write_host_prefix_, read_hosts_prefix_, db_name_prefix_;
    int write_port_, read_port_;
    std::string user_, password_;
    int write_pool_size_;
};

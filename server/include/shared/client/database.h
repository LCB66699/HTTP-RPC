// MySQL 璇诲啓鍒嗙灞? 涓诲簱鍐?杩炴帴姹? + 浠庡簱璇?杩炴帴姹?round-robin)
// libmysqlclient C API via TCP 3306
#pragma once
#include <mysql/mysql.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

// ---- MYSQL 鏅鸿兘鎸囬拡 鈥?鏋愭瀯鏃惰嚜鍔?mysql_close锛屾秷闄ゆ墜鍔ㄥ叧闂?----

struct MysqlDeleter {
    void operator()(MYSQL *c) const {
        if (c) mysql_close(c);
    }
};
using MysqlPtr = std::unique_ptr<MYSQL, MysqlDeleter>;

// ---- SQL 鍙傛暟瀹夊叏鍖呰 鈥?缂栬瘧鏈熷己鍒惰浆涔夛紝鏉滅粷浜哄伐婕忓啓 q() ----

class sql_param {
   public:
    // 瀛楃涓叉瀯閫?鈥?寮哄埗 mysql_real_escape_string锛岀紪璇戞湡鍏滃簳
    sql_param(MYSQL *conn, const std::string &raw);

    // 鏁板€兼瀯閫?鈥?鐩存帴杞瓧绗︿覆锛屾棤闇€杞箟/鍔犲紩鍙?
    sql_param(int64_t val) : val_(std::to_string(val)), quote_(false) {}
    sql_param(int val) : val_(std::to_string(val)), quote_(false) {}
    sql_param(unsigned long val) : val_(std::to_string(val)), quote_(false) {}

    const std::string &str() const { return val_; }
    bool needs_quote() const { return quote_; }

    // 鎷煎叆 SQL 鐨勬渶缁堝舰鎬侊細瀛楃涓茶嚜鍔ㄥ姞鍗曞紩鍙凤紝鏁板€艰８鎷?
    std::string sql() const { return quote_ ? "'" + val_ + "'" : val_; }

   private:
    std::string val_;
    bool quote_ = true;
};

// 绫诲瀷瀹夊叏 SQL 鏋勫缓鍣?
// 鐢ㄦ硶: make_sql("INSERT INTO t (a,b) VALUES ({},{})", sql_param(conn, str), 42)
// 浠讳綍闈?sql_param 鐨勯潪绠楁暟绫诲瀷锛堝 std::string / const char*锛夐兘浼氳Е鍙?
// static_assert 缂栬瘧閿欒
template <typename... Args>
std::string make_sql(const std::string &tmpl, const Args &...args) {
    std::ostringstream oss;
    size_t pos = 0;
    auto append = [&](const auto &arg) {
        size_t ph = tmpl.find("{}", pos);
        if (ph != std::string::npos) {
            oss << tmpl.substr(pos, ph - pos);
            pos = ph + 2;
        }
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, sql_param>) {
            oss << arg.sql();
        } else if constexpr (std::is_arithmetic_v<T>) {
            oss << arg;
        } else {
            static_assert(sizeof(T) == 0,
                          "make_sql: string argument must be wrapped in sql_param(conn, str)");
        }
    };
    (append(args), ...);
    oss << tmpl.substr(pos);
    return oss.str();
}

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
    std::string storage_path;  // non-empty 鈫?content lives in object storage, not
                               // file_content
    int version = 1;
};

class Database {
   public:
    // write_host: MySQL master
    // read_hosts: 浠庡簱鍦板潃, 閫楀彿鍒嗛殧
    // write_pool_size: 鍐欒繛鎺ユ睜澶у皬, 榛樿4; 璁句负1鍗抽€€鍖栦负鍘熷崟杩炴帴妯″紡
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
    int64_t GetUserIdByPhone(const std::string &phone);
    std::string GetUsernameById(int64_t user_id);
    bool VerifyUserPassword(int64_t user_id, const std::string &password);
    bool UpdateUserPassword(int64_t user_id, const std::string &new_hash);

    // Spreadsheets 鈥?caller passes user_id (users.id) as the owner key.
    // username is stored alongside for display; idempotency_key deduplicates
    // retries.
    bool CreateSpreadsheet(int64_t user_id, const std::string &username, const std::string &name,
                           const std::string &desc, const std::string &headers_json, const std::string &data_json,
                           int64_t &out_id, const std::string &idempotency_key = "");
    bool GetSpreadsheet(int64_t id, int64_t user_id, SpreadsheetRow &out);
    // page is 0-based; page_size=0 disables pagination and returns all rows
    // (backward compat)
    bool ListSpreadsheets(int64_t user_id, std::vector<SpreadsheetSummary> &out,
                          std::string &next_cursor, bool &has_more,
                          int limit = 20, const std::string &cursor = "");
    bool UpdateSpreadsheet(int64_t id, int64_t user_id, const std::string &name, const std::string &desc,
                           const std::string &headers_json, const std::string &data_json, int version = 0);
    bool UpdateSpreadsheet(int64_t id, const std::string &name, const std::string &desc,
                           const std::string &headers_json, const std::string &data_json, int version = 0);
    bool DeleteSpreadsheet(int64_t id, int64_t user_id = 0);
    // Returns the owner's user_id and optionally the current version for
    // optimistic locking.
    bool GetSpreadsheetOwner(int64_t id, int64_t &owner_user_id, int *out_version = nullptr);

    // Files 鈥?same user_id convention.
    // storage_key: object-storage path returned by MinIO PutObject (empty =
    // legacy BLOB mode).
    bool CreateFile(int64_t user_id, const std::string &username, const std::string &original_name, int64_t size,
                    const std::string &mime_type, const std::string &storage_key, int64_t &out_id,
                    const std::string &idempotency_key = "");
    bool UpdateFileContent(int64_t id, const std::string &content, int version = 0);
    bool GetFile(int64_t id, int64_t user_id, FileRow &out);
    // page is 0-based; page_size=0 disables pagination and returns all rows
    // (backward compat)
    bool ListFiles(int64_t user_id, std::vector<FileRow> &out,
                   std::string &next_cursor, bool &has_more,
                   int limit = 20, const std::string &cursor = "");
    bool DeleteFile(int64_t id, int64_t user_id = 0);
    bool GetFileOwner(int64_t id, int64_t &owner_user_id);
    bool GetFileStoragePath(int64_t id, std::string &storage_path);
    bool GetSpreadsheetStoragePath(int64_t id, std::string &storage_path);
    bool UpdateSpreadsheetStoragePath(int64_t id, const std::string &storage_path);
    bool InsertOutbox(const std::string &event_type, const std::string &payload,
                      const std::string &trace_context = "");
    bool CreateFolder(int64_t user_id, const std::string &name, int64_t parent_id, int64_t &out_id);
    bool MoveFile(int64_t id, int64_t target_folder_id, int version = 0);
    int BatchDeleteFiles(int64_t user_id, const std::vector<int64_t> &ids);

    // Resource sharing
    bool CreateResourceShare(int64_t owner_id, const std::string &resource_type, int64_t resource_id,
                             const std::string &grantee_username, const std::string &permission);
    bool RevokeResourceShare(int64_t owner_id, const std::string &resource_type, int64_t resource_id,
                             const std::string &grantee_username);
    bool ListResourceShares(int64_t owner_id, const std::string &resource_type, int64_t resource_id,
                            std::string &out_entries_json);
    bool CheckShareAccess(const std::string &username, const std::string &resource_type, int64_t resource_id,
                          std::string &out_permission);
    bool CreateShareLink(int64_t owner_id, const std::string &resource_type, int64_t resource_id,
                         const std::string &permission, std::string &out_token);
    bool GetShareLinkByToken(const std::string &token, std::string &resource_type, int64_t &resource_id,
                             std::string &permission, int64_t &owner_id);
    bool EnsureSharingTables();

    // Workspace
    bool EnsureWorkspaceTables();
    bool CreateWorkspace(int64_t owner_id, const std::string &name, int64_t &out_id);
    bool GetWorkspace(int64_t id, std::string &name, int64_t &owner_id, std::string &created_at);
    bool ListWorkspaces(int64_t user_id, std::string &out_json);
    bool UpdateWorkspace(int64_t id, const std::string &name);
    bool DeleteWorkspace(int64_t id);
    bool AddWorkspaceMember(int64_t workspace_id, int64_t user_id, const std::string &username, const std::string &role);
    bool RemoveWorkspaceMember(int64_t workspace_id, int64_t user_id);
    bool IsWorkspaceOwner(int64_t workspace_id, int64_t user_id);
    bool GetWorkspaceMemberRole(int64_t workspace_id, int64_t user_id, std::string &out_role);

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
    MYSQL *GetConnection() { return write_conns_.empty() ? nullptr : write_conns_[0]->conn.get(); }

    // 鍋ュ悍妫€鏌ワ細鍚庡彴绾跨▼姣?30s PING 杩炴帴姹狅紝鑷姩閲嶅缓姝昏繛鎺?+ 娣樻卑绌洪棽杩炴帴
    void StartHealthCheck();
    void StopHealthCheck();
    void SetMinIdle(int n) { min_idle_.store(n); }
    void SetIdleTimeoutSec(int sec) { idle_timeout_ms_.store(sec * 1000); }

   private:
    std::string user_, password_, db_name_;
    std::string write_host_;  // 璁板綍涓诲簱host渚涢噸杩?
    int write_port_, read_port_;
    int pool_size_;
    std::atomic<int> min_idle_{2};              // 鏈€浣庝繚鐣欒繛鎺ユ暟锛屼綆浜庢鏁颁笉娣樻卑
    std::atomic<int> idle_timeout_ms_{300000};   // 绌洪棽瓒呮椂(ms)锛岄粯璁?5min
    std::vector<std::string> read_hosts_;
    Snowflake *snowflake_ = nullptr;

    // 杩炴帴鍗曞厓 鈥?璇诲啓杩炴帴姹犲叡鐢ㄦ缁撴瀯
    struct PoolConn {
        MysqlPtr conn;                          // 鏅鸿兘鎸囬拡锛宺eset/璧嬪€艰嚜鍔?mysql_close 鏃ц繛鎺?
        std::mutex mtx;
        std::atomic<int64_t> last_used_ms{0};  // 鏈€鍚庝竴娆″綊杩樼殑鏃堕棿鎴?ms)锛孒ealthLoop 璇诲彇鏃犻渶鍔犻攣
    };

    // 鍐欒繛鎺ユ睜锛堜富搴? 澶氳繛鎺ュ苟琛屽啓锛?
    std::vector<std::unique_ptr<PoolConn>> write_conns_;
    std::atomic<size_t> write_idx_{0};

    // 璇昏繛鎺ユ睜锛堜粠搴? round-robin 鏃犻攣鍒嗗彂锛?
    std::vector<std::unique_ptr<PoolConn>> read_conns_;
    std::atomic<size_t> read_idx_{0};

    MysqlPtr ConnectMYSQL(const std::string &host, int port);
    MYSQL *EscConn();  // 杩斿洖涓€涓彲鐢ㄨ繛鎺? 浠呬緵 mysql_real_escape_string() 鐢?
    bool ExecWrite(const std::string &sql);
    bool ExecWriteInsert(const std::string &sql, int64_t &out_id);
    MYSQL *GetReadConn();
    bool RunQuery(MYSQL *conn, const std::string &sql, MYSQL_RES **out_res);
    bool ExecRead(const std::string &sql, std::function<bool(MYSQL_RES *)> handler);

    // 杩炴帴鍙ユ焺 鈥?鎸侀攣 + 瑁告寚閽堬紝鏋愭瀯鑷姩瑙ｉ攣
    struct ConnHandle {
        std::unique_lock<std::mutex> lock;
        MYSQL *conn = nullptr;
        explicit operator bool() const { return conn != nullptr; }
    };
    ConnHandle GetHealthyWriteConn();
    ConnHandle GetHealthyReadConn();

    std::thread health_check_;
    std::atomic<bool> health_running_{false};
    void HealthLoop();
};

#include "shared/base/service_interfaces.h"

// ---- ShardedDatabase: hash-routes by user_id across N Database shards ----
class ShardedDatabase : public IDatabase {
   public:
    // shard_count: 鍒嗙墖鏁? host_prefix: "mysql-spreadsheet" 鈫?
    // "mysql-spreadsheet-0","mysql-spreadsheet-1"... db_name_prefix:
    // "rpc_spreadsheet" 鈫?"rpc_spreadsheet_0","rpc_spreadsheet_1"...
    ShardedDatabase(int shard_count, const std::string &write_host_prefix, int write_port,
                    const std::string &read_hosts_prefix, int read_port, const std::string &user,
                    const std::string &password, const std::string &db_name_prefix, int write_pool_size = 4);

    void InitializeAll();
    void SetSnowflake(Snowflake *sf) {
        for (auto &db : shards_)
            db->SetSnowflake(sf);
    }

    // === Users (auth 鈥?single shard) ===
    bool AddUser(const std::string &username, const std::string &password_hash);
    bool GetUser(const std::string &username, std::string &password_hash);
    bool UserExists(const std::string &username);
    int GetTokenVersion(const std::string &username);
    bool IncrementTokenVersion(const std::string &username);
    int64_t GetUserId(const std::string &username);
    int64_t GetUserIdByPhone(const std::string &phone);
    std::string GetUsernameById(int64_t user_id);
    bool VerifyUserPassword(int64_t user_id, const std::string &password);
    bool UpdateUserPassword(int64_t user_id, const std::string &new_hash);
    bool ImportFromUsersJson(const std::string &json_path);

    // === Spreadsheets (hash by user_id) ===
    bool CreateSpreadsheet(int64_t user_id, const std::string &username, const std::string &name,
                           const std::string &desc, const std::string &headers_json, const std::string &data_json,
                           int64_t &out_id, const std::string &idempotency_key = "");
    bool GetSpreadsheet(int64_t id, int64_t user_id, SpreadsheetRow &out);
    bool ListSpreadsheets(int64_t user_id, std::vector<SpreadsheetSummary> &out,
                          std::string &next_cursor, bool &has_more,
                          int limit = 20, const std::string &cursor = "");
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
    bool UpdateFileContent(int64_t id, const std::string &content, int version = 0);
    bool GetFile(int64_t id, int64_t user_id, FileRow &out);
    bool ListFiles(int64_t user_id, std::vector<FileRow> &out,
                   std::string &next_cursor, bool &has_more,
                   int limit = 20, const std::string &cursor = "");
    bool DeleteFile(int64_t id, int64_t user_id = 0);
    bool GetFileOwner(int64_t id, int64_t &owner_user_id);
    bool GetFileStoragePath(int64_t id, std::string &storage_path);
    bool GetSpreadsheetStoragePath(int64_t id, std::string &storage_path);
    bool UpdateSpreadsheetStoragePath(int64_t id, const std::string &storage_path);
    bool InsertOutbox(int64_t user_id, const std::string &event_type, const std::string &payload,
                      const std::string &trace_context = "");
    bool CreateFolder(int64_t user_id, const std::string &name, int64_t parent_id, int64_t &out_id);
    bool MoveFile(int64_t id, int64_t target_folder_id, int version = 0);
    int BatchDeleteFiles(int64_t user_id, const std::vector<int64_t> &ids);

    // === Resource Sharing (auth 鈥?single shard) ===
    bool CreateResourceShare(int64_t owner_id, const std::string &resource_type, int64_t resource_id,
                             const std::string &grantee_username, const std::string &permission) {
        return shards_[0]->CreateResourceShare(owner_id, resource_type, resource_id, grantee_username, permission);
    }
    bool RevokeResourceShare(int64_t owner_id, const std::string &resource_type, int64_t resource_id,
                             const std::string &grantee_username) {
        return shards_[0]->RevokeResourceShare(owner_id, resource_type, resource_id, grantee_username);
    }
    bool ListResourceShares(int64_t owner_id, const std::string &resource_type, int64_t resource_id,
                            std::string &out_entries_json) {
        return shards_[0]->ListResourceShares(owner_id, resource_type, resource_id, out_entries_json);
    }
    bool CheckShareAccess(const std::string &username, const std::string &resource_type, int64_t resource_id,
                          std::string &out_permission) {
        return shards_[0]->CheckShareAccess(username, resource_type, resource_id, out_permission);
    }
    bool CreateShareLink(int64_t owner_id, const std::string &resource_type, int64_t resource_id,
                         const std::string &permission, std::string &out_token) {
        return shards_[0]->CreateShareLink(owner_id, resource_type, resource_id, permission, out_token);
    }
    bool GetShareLinkByToken(const std::string &token, std::string &resource_type, int64_t &resource_id,
                             std::string &permission, int64_t &owner_id) {
        return shards_[0]->GetShareLinkByToken(token, resource_type, resource_id, permission, owner_id);
    }
    bool EnsureSharingTables() {
        return shards_[0]->EnsureSharingTables();
    }

    bool EnsureWorkspaceTables()    { return shards_[0]->EnsureWorkspaceTables(); }
    bool CreateWorkspace(int64_t owner_id, const std::string &name, int64_t &out_id) { return shards_[0]->CreateWorkspace(owner_id, name, out_id); }
    bool GetWorkspace(int64_t id, std::string &name, int64_t &owner_id, std::string &created_at) { return shards_[0]->GetWorkspace(id, name, owner_id, created_at); }
    bool ListWorkspaces(int64_t user_id, std::string &out_json) { return shards_[0]->ListWorkspaces(user_id, out_json); }
    bool UpdateWorkspace(int64_t id, const std::string &name) { return shards_[0]->UpdateWorkspace(id, name); }
    bool DeleteWorkspace(int64_t id) { return shards_[0]->DeleteWorkspace(id); }
    bool AddWorkspaceMember(int64_t wid, int64_t uid, const std::string &uname, const std::string &role) { return shards_[0]->AddWorkspaceMember(wid, uid, uname, role); }
    bool RemoveWorkspaceMember(int64_t wid, int64_t uid) { return shards_[0]->RemoveWorkspaceMember(wid, uid); }
    bool IsWorkspaceOwner(int64_t wid, int64_t uid) { return shards_[0]->IsWorkspaceOwner(wid, uid); }
    bool GetWorkspaceMemberRole(int64_t wid, int64_t uid, std::string &role) { return shards_[0]->GetWorkspaceMemberRole(wid, uid, role); }

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
    void SetMinIdle(int n) {
        for (auto &db : shards_)
            db->SetMinIdle(n);
    }
    void SetIdleTimeoutSec(int sec) {
        for (auto &db : shards_)
            db->SetIdleTimeoutSec(sec);
    }

    int ShardCount() const { return shard_count_; }
    Database *ShardFor(int64_t user_id);
    Database *ShardForBroadcast();  // 杩斿洖绗竴涓垎鐗囷紙鐢ㄤ簬骞挎挱绫绘煡璇級

    Database *AuthDB() { return shards_[0].get(); }

   private:
    int shard_count_;
    std::vector<std::unique_ptr<Database>> shards_;
    std::string write_host_prefix_, read_hosts_prefix_, db_name_prefix_;
    int write_port_, read_port_;
    std::string user_, password_;
    int write_pool_size_;
};

#include "database.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include "sha256.h"
#include "snowflake.h"

// ---- connection helpers ----

MYSQL *Database::ConnectMYSQL(const std::string &host, int port) {
    MYSQL *c = mysql_init(nullptr);
    if (!c) {
        fprintf(stderr, "[DB] mysql_init failed\n");
        return nullptr;
    }
    if (!mysql_real_connect(c, host.c_str(), user_.c_str(), password_.c_str(), db_name_.c_str(), port, nullptr, 0)) {
        fprintf(stderr, "[DB] connect %s:%d failed: %s\n", host.c_str(), port, mysql_error(c));
        mysql_close(c);
        return nullptr;
    }
    return c;
}

bool Database::RunQuery(MYSQL *conn, const std::string &sql, MYSQL_RES **out_res) {
    if (mysql_query(conn, sql.c_str()) == 0) {
        *out_res = mysql_store_result(conn);
        return true;
    }
    unsigned int err = mysql_errno(conn);
    fprintf(stderr, "[DB] SQL error %u: %s\n", err, mysql_error(conn));
    *out_res = nullptr;
    return false;
}

// 转义辅助 — mysql_real_escape_string 是纯客户端函数, 同 charset
// 下任意连接结果一致
static std::string escape(MYSQL *conn, const std::string &s) {
    if (!conn)
        return s;
    std::string out(s.size() * 2 + 1, '\0');
    unsigned long len = mysql_real_escape_string(conn, &out[0], s.c_str(), s.size());
    out.resize(len);
    return out;
}
static std::string q(MYSQL *conn, const std::string &s) {
    return "'" + escape(conn, s) + "'";
}

// 取任意可用连接供转义用（遍历写池→读池, 返回首个存活连接）
MYSQL *Database::EscConn() {
    for (auto &wc : write_conns_)
        if (wc->conn)
            return wc->conn;
    for (auto &rc : read_conns_)
        if (rc->conn)
            return rc->conn;
    return nullptr;
}

Database::Database(const std::string &write_host, int write_port, const std::string &read_hosts, int read_port,
                   const std::string &user, const std::string &password, const std::string &db_name,
                   int write_pool_size)
    : user_(user),
      password_(password),
      db_name_(db_name),
      write_host_(write_host),
      write_port_(write_port),
      read_port_(read_port),
      pool_size_(write_pool_size) {
    // 拆分读地址
    std::string hosts = read_hosts;
    size_t start = 0, end;
    while ((end = hosts.find(',', start)) != std::string::npos) {
        read_hosts_.push_back(hosts.substr(start, end - start));
        start = end + 1;
    }
    read_hosts_.push_back(hosts.substr(start));

    // 创建写连接池 — 同一主库多条连接, 实现并行写
    write_conns_.reserve(pool_size_);
    for (int i = 0; i < pool_size_; i++) {
        auto wc = std::make_unique<PoolConn>();
        wc->conn = ConnectMYSQL(write_host_, write_port_);
        if (!wc->conn)
            fprintf(stderr, "[DB] WARNING: write pool conn %d/%d failed\n", i + 1, pool_size_);
        write_conns_.push_back(std::move(wc));
    }

    // 创建读连接池（从库）
    for (auto &h : read_hosts_) {
        MYSQL *c = ConnectMYSQL(h, read_port_);
        if (c) {
            auto rc = std::make_unique<PoolConn>();
            rc->conn = c;
            read_conns_.push_back(std::move(rc));
        } else {
            fprintf(stderr, "[DB] WARNING: read slave %s:%d unavailable\n", h.c_str(), read_port_);
        }
    }
    printf("[DB] write=%s:%d pool=%d reads=%zu slaves\n", write_host_.c_str(), write_port_, pool_size_,
           read_conns_.size());
}

Database::~Database() {
    StopHealthCheck();
    for (auto &wc : write_conns_)
        if (wc->conn)
            mysql_close(wc->conn);
    for (auto &rc : read_conns_)
        if (rc->conn)
            mysql_close(rc->conn);
}

// ---- 写操作（主库连接池 round-robin 分发, 每连接独立 mutex） ----

bool Database::ExecWrite(const std::string &sql) {
    if (write_conns_.empty())
        return false;
    // 原子递增取索引, 无锁分发到不同连接
    size_t idx = write_idx_.fetch_add(1, std::memory_order_relaxed) % write_conns_.size();
    auto &wc = write_conns_[idx];
    // 只锁被选中的连接, 其他连接不受影响
    std::lock_guard<std::mutex> lock(wc->mtx);
    if (!wc->conn)
        return false;
    if (mysql_query(wc->conn, sql.c_str()) == 0)
        return true;
    unsigned int err = mysql_errno(wc->conn);
    fprintf(stderr, "[DB:write#%zu] error %u: %s\n", idx, err, mysql_error(wc->conn));
    if (err == CR_SERVER_LOST || err == CR_SERVER_GONE_ERROR) {
        fprintf(stderr, "[DB:write#%zu] reconnecting...\n", idx);
        mysql_close(wc->conn);
        wc->conn = ConnectMYSQL(write_host_, write_port_);
    }
    return false;
}

// 写 + 返回自增ID: INSERT 和 mysql_insert_id 在同一连接同一锁内原子完成
bool Database::ExecWriteInsert(const std::string &sql, int64_t &out_id) {
    if (write_conns_.empty())
        return false;
    size_t idx = write_idx_.fetch_add(1, std::memory_order_relaxed) % write_conns_.size();
    auto &wc = write_conns_[idx];
    std::lock_guard<std::mutex> lock(wc->mtx);
    if (!wc->conn)
        return false;
    if (mysql_query(wc->conn, sql.c_str()) != 0) {
        fprintf(stderr, "[DB:write#%zu] ExecWriteInsert error: %s\n", idx, mysql_error(wc->conn));
        return false;
    }
    out_id = mysql_insert_id(wc->conn);
    return true;
}

// ---- 读操作（从库轮询, 无从库时回退到写池） ----

MYSQL *Database::GetReadConn() {
    if (read_conns_.empty()) {
        for (auto &wc : write_conns_)
            if (wc->conn)
                return wc->conn;
        return nullptr;
    }
    size_t idx = read_idx_.fetch_add(1, std::memory_order_relaxed) % read_conns_.size();
    return read_conns_[idx]->conn;
}

bool Database::ExecRead(const std::string &sql, std::function<bool(MYSQL_RES *)> handler) {
    // 有从库: 走从库
    if (!read_conns_.empty()) {
        size_t idx = read_idx_.fetch_add(1, std::memory_order_relaxed) % read_conns_.size();
        auto &rc = read_conns_[idx];
        std::lock_guard<std::mutex> lock(rc->mtx);
        if (rc->conn) {
            MYSQL_RES *res = nullptr;
            if (RunQuery(rc->conn, sql, &res)) {
                bool ok = handler(res);
                if (res)
                    mysql_free_result(res);
                return ok;
            }
        }
    }
    // 无从库 → 回退到写池
    if (!write_conns_.empty()) {
        size_t idx = write_idx_.fetch_add(1, std::memory_order_relaxed) % write_conns_.size();
        auto &wc = write_conns_[idx];
        std::lock_guard<std::mutex> lock(wc->mtx);
        if (wc->conn) {
            MYSQL_RES *res = nullptr;
            if (RunQuery(wc->conn, sql, &res)) {
                bool ok = handler(res);
                if (res)
                    mysql_free_result(res);
                return ok;
            }
        }
    }
    return false;
}

// ---- schema init（首连接执行, DDL 全局生效无需在所有连接上重复） ----

bool Database::Initialize() {
    if (write_conns_.empty())
        return false;

    // 启动时序可能导致连接为 null，尝试重连所有写连接
    for (size_t i = 0; i < write_conns_.size(); ++i) {
        if (!write_conns_[i]->conn) {
            write_conns_[i]->conn = ConnectMYSQL(write_host_, write_port_);
            if (i == 0 && !write_conns_[0]->conn)
                return false;
        }
    }

    // 确保 MySQL 完全就绪（健康检查 ping 通过不代表能接受 DDL）
    if (mysql_ping(write_conns_[0]->conn) != 0) {
        fprintf(stderr, "[DB] mysql_ping failed after connect\n");
        return false;
    }

    // 锁首连接执行全部 DDL, 避免与 ExecWrite 的 round-robin 分配交叉
    auto &wc = write_conns_[0];
    std::lock_guard<std::mutex> lock(wc->mtx);
    MYSQL *c = wc->conn;

    auto exec = [&](const char *sql) {
        if (mysql_query(c, sql) != 0)
            fprintf(stderr, "[DB] DDL error: %s\n", mysql_error(c));
    };

    exec(
        "CREATE TABLE IF NOT EXISTS users ("
        "id BIGINT AUTO_INCREMENT PRIMARY KEY, "
        "username VARCHAR(64) UNIQUE NOT NULL, "
        "password_hash VARCHAR(256) NOT NULL, "
        "created_at DATETIME NOT NULL DEFAULT NOW())");
    exec(
        "CREATE TABLE IF NOT EXISTS spreadsheets ("
        "id BIGINT AUTO_INCREMENT PRIMARY KEY, "
        "username VARCHAR(64) NOT NULL, "
        "name VARCHAR(255) NOT NULL, "
        "description TEXT, "
        "headers_json JSON NOT NULL, "
        "data_json JSON NOT NULL, "
        "row_count INT NOT NULL DEFAULT 0, "
        "col_count INT NOT NULL DEFAULT 0, "
        "created_at DATETIME NOT NULL DEFAULT NOW(), "
        "updated_at DATETIME NOT NULL DEFAULT NOW())");
    exec(
        "CREATE TABLE IF NOT EXISTS files ("
        "id BIGINT AUTO_INCREMENT PRIMARY KEY, "
        "username VARCHAR(64) NOT NULL, "
        "original_name VARCHAR(512) NOT NULL, "
        "size BIGINT NOT NULL DEFAULT 0, "
        "mime_type VARCHAR(128) DEFAULT '', "
        "file_content LONGBLOB, "
        "created_at DATETIME NOT NULL DEFAULT NOW())");
    exec(
        "CREATE TABLE IF NOT EXISTS outbox ("
        "id BIGINT AUTO_INCREMENT PRIMARY KEY, "
        "event_type VARCHAR(64) NOT NULL, "
        "payload JSON NOT NULL, "
        "created_at DATETIME DEFAULT NOW())");

    // indexes + migrations (幂等, 失败忽略)
    mysql_query(c, "ALTER TABLE spreadsheets ADD COLUMN version INT NOT NULL DEFAULT 1");
    mysql_query(c, "ALTER TABLE users MODIFY COLUMN password_hash VARCHAR(256) NOT NULL");
    mysql_query(c, "ALTER TABLE users ADD COLUMN token_version INT NOT NULL DEFAULT 0");
    exec(
        "CREATE TABLE IF NOT EXISTS share_permissions ("
        "id BIGINT AUTO_INCREMENT PRIMARY KEY, "
        "resource_type VARCHAR(20) NOT NULL, "
        "resource_id BIGINT NOT NULL, "
        "owner_id BIGINT NOT NULL, "
        "grantee_id BIGINT NOT NULL, "
        "permission VARCHAR(20) NOT NULL DEFAULT 'view', "
        "created_at DATETIME DEFAULT NOW())");
    exec(
        "CREATE TABLE IF NOT EXISTS share_links ("
        "id BIGINT AUTO_INCREMENT PRIMARY KEY, "
        "resource_type VARCHAR(20) NOT NULL, "
        "resource_id BIGINT NOT NULL, "
        "token VARCHAR(64) UNIQUE NOT NULL, "
        "permission VARCHAR(20) NOT NULL DEFAULT 'view', "
        "expires_at DATETIME NULL, "
        "created_at DATETIME DEFAULT NOW())");
    mysql_query(c, "ALTER TABLE users ADD COLUMN phone VARCHAR(20) UNIQUE NULL");
    mysql_query(c, "ALTER TABLE files ADD COLUMN folder_id BIGINT DEFAULT 0");
    mysql_query(c, "ALTER TABLE files ADD COLUMN is_folder BOOLEAN DEFAULT FALSE");
    mysql_query(c, "ALTER TABLE users ADD COLUMN display_name VARCHAR(100) DEFAULT NULL");
    mysql_query(c, "ALTER TABLE users ADD COLUMN avatar_url TEXT DEFAULT NULL");
    mysql_query(c, "ALTER TABLE files ADD COLUMN file_content LONGBLOB AFTER mime_type");
    mysql_query(c, "ALTER TABLE files DROP COLUMN stored_name");

    // Object-storage migration: track where file content lives on disk/object
    // store
    mysql_query(c, "ALTER TABLE files ADD COLUMN storage_path VARCHAR(512) DEFAULT NULL");

    // Trace context for distributed tracing through the outbox
    mysql_query(c, "ALTER TABLE outbox ADD COLUMN trace_context VARCHAR(512) DEFAULT NULL AFTER payload");

    // Idempotency key columns — NULL means "no deduplication" (multiple NULLs
    // allowed by UNIQUE). ON DUPLICATE KEY UPDATE id=LAST_INSERT_ID(id) returns
    // the existing id on replay.
    mysql_query(c,
                "ALTER TABLE spreadsheets ADD COLUMN idempotency_key CHAR(36) "
                "UNIQUE NULL DEFAULT NULL");
    mysql_query(c,
                "ALTER TABLE files        ADD COLUMN idempotency_key CHAR(36) "
                "UNIQUE NULL DEFAULT NULL");

    // user_id columns — integer FK to users.id; avoids username-as-FK design
    // flaw. DEFAULT 0 allows idempotent migration on existing rows (populated
    // below via JOIN).
    mysql_query(c, "ALTER TABLE spreadsheets ADD COLUMN user_id BIGINT NOT NULL DEFAULT 0");
    mysql_query(c, "ALTER TABLE files        ADD COLUMN user_id BIGINT NOT NULL DEFAULT 0");
    mysql_query(c,
                "ALTER TABLE spreadsheets ADD COLUMN storage_path "
                "VARCHAR(512) DEFAULT NULL");
    // Backfill from users table (no-op if already populated).
    mysql_query(c,
                "UPDATE spreadsheets s JOIN users u ON s.username=u.username "
                "SET s.user_id=u.id WHERE s.user_id=0");
    mysql_query(c,
                "UPDATE files f        JOIN users u ON f.username=u.username "
                "SET f.user_id=u.id WHERE f.user_id=0");

    // Replace single-column indexes with composite covering indexes on user_id +
    // sort column. DROP first (idempotent — fails silently if already removed),
    // then CREATE.
    mysql_query(c, "ALTER TABLE spreadsheets DROP INDEX idx_sheets_username");
    mysql_query(c, "ALTER TABLE spreadsheets DROP INDEX idx_sheets_user_time");
    mysql_query(c,
                "CREATE INDEX idx_sheets_user_time ON spreadsheets(user_id, "
                "updated_at DESC)");

    mysql_query(c, "ALTER TABLE files DROP INDEX idx_files_username");
    mysql_query(c, "ALTER TABLE files DROP INDEX idx_files_user_time");
    mysql_query(c, "CREATE INDEX idx_files_user_time ON files(user_id, created_at DESC)");

    // undo_log: composite (xid, id DESC) eliminates the filesort on ORDER BY id
    // DESC LIMIT 1. Add created_at index to accelerate the periodic purge DELETE.
    mysql_query(c, "ALTER TABLE undo_log DROP INDEX idx_undo_xid");
    mysql_query(c, "CREATE INDEX idx_undo_xid_id ON undo_log(xid, id DESC)");
    mysql_query(c, "CREATE INDEX idx_undo_created ON undo_log(created_at)");

    // 覆盖索引 — SELECT token_version FROM users WHERE username=X 避免回表
    mysql_query(c, "ALTER TABLE users DROP INDEX IF EXISTS username");
    mysql_query(c,
                "ALTER TABLE users ADD UNIQUE INDEX idx_users_user_ver "
                "(username, token_version)");

    // INT → BIGINT 升级：兼容已有表，MySQL 8.0 支持 ALGORITHM=INSTANT
    mysql_query(c, "ALTER TABLE users MODIFY COLUMN id BIGINT AUTO_INCREMENT");
    mysql_query(c, "ALTER TABLE spreadsheets MODIFY COLUMN id BIGINT AUTO_INCREMENT");
    mysql_query(c,
                "ALTER TABLE spreadsheets MODIFY COLUMN user_id BIGINT NOT "
                "NULL DEFAULT 0");
    mysql_query(c, "ALTER TABLE files MODIFY COLUMN id BIGINT AUTO_INCREMENT");
    mysql_query(c, "ALTER TABLE files MODIFY COLUMN user_id BIGINT NOT NULL DEFAULT 0");
    mysql_query(c, "ALTER TABLE undo_log MODIFY COLUMN id BIGINT AUTO_INCREMENT");
    mysql_query(c,
                "ALTER TABLE spreadsheets MODIFY COLUMN idempotency_key "
                "CHAR(36) NULL DEFAULT NULL");
    mysql_query(c,
                "ALTER TABLE files MODIFY COLUMN idempotency_key CHAR(36) "
                "NULL DEFAULT NULL");

    printf("[DB] schema initialized on master: %s\n", db_name_.c_str());
    return true;
}

// ---- Users ----

bool Database::AddUser(const std::string &username, const std::string &password_hash) {
    MYSQL *ec = EscConn();
    int64_t snowflake_id = snowflake_ ? snowflake_->Next() : 0;
    std::string sql = "INSERT INTO users (id, username, password_hash) VALUES (" + std::to_string(snowflake_id) + "," +
                      q(ec, username) + ", " + q(ec, password_hash) + ")";
    return ExecWrite(sql);
}

bool Database::GetUser(const std::string &username, std::string &password_hash_out) {
    // 强一致: 密码验证必须读主库
    if (write_conns_.empty())
        return false;
    size_t idx = write_idx_.fetch_add(1, std::memory_order_relaxed) % write_conns_.size();
    auto &wc = write_conns_[idx];
    std::lock_guard<std::mutex> lock(wc->mtx);
    if (!wc->conn)
        return false;
    std::string sql = "SELECT password_hash FROM users WHERE username=" + q(wc->conn, username);
    MYSQL_RES *res = nullptr;
    if (!RunQuery(wc->conn, sql, &res))
        return false;
    MYSQL_ROW row = mysql_fetch_row(res);
    bool ok = row != nullptr;
    if (ok)
        password_hash_out = row[0] ? row[0] : "";
    mysql_free_result(res);
    return ok;
}

bool Database::UserExists(const std::string &username) {
    // 强一致: 查重必须走主库, 避免主从延迟导致重复注册
    if (write_conns_.empty())
        return false;
    size_t idx = write_idx_.fetch_add(1, std::memory_order_relaxed) % write_conns_.size();
    auto &wc = write_conns_[idx];
    std::lock_guard<std::mutex> lock(wc->mtx);
    if (!wc->conn)
        return false;
    std::string sql = "SELECT 1 FROM users WHERE username=" + q(wc->conn, username);
    MYSQL_RES *res = nullptr;
    if (!RunQuery(wc->conn, sql, &res))
        return false;
    bool exists = mysql_fetch_row(res) != nullptr;
    mysql_free_result(res);
    return exists;
}

int Database::GetTokenVersion(const std::string &username) {
    // 强一致: token 版本号必须读主库
    if (write_conns_.empty())
        return -1;
    size_t idx = write_idx_.fetch_add(1, std::memory_order_relaxed) % write_conns_.size();
    auto &wc = write_conns_[idx];
    std::lock_guard<std::mutex> lock(wc->mtx);
    if (!wc->conn)
        return -1;
    std::string sql = "SELECT token_version FROM users WHERE username=" + q(wc->conn, username);
    MYSQL_RES *res = nullptr;
    if (!RunQuery(wc->conn, sql, &res))
        return -1;
    MYSQL_ROW row = mysql_fetch_row(res);
    int ver = (row && row[0]) ? std::stoi(row[0]) : -1;
    mysql_free_result(res);
    return ver;
}

bool Database::IncrementTokenVersion(const std::string &username) {
    MYSQL *ec = EscConn();
    std::string sql = "UPDATE users SET token_version = token_version + 1 WHERE username=" + q(ec, username);
    return ExecWrite(sql);
}

int64_t Database::GetUserId(const std::string &username) {
    // Must read master to avoid replication lag causing stale id on fresh
    // registration.
    if (write_conns_.empty())
        return -1;
    size_t idx = write_idx_.fetch_add(1, std::memory_order_relaxed) % write_conns_.size();
    auto &wc = write_conns_[idx];
    std::lock_guard<std::mutex> lock(wc->mtx);
    if (!wc->conn)
        return -1;
    std::string sql = "SELECT id FROM users WHERE username=" + q(wc->conn, username);
    MYSQL_RES *res = nullptr;
    if (!RunQuery(wc->conn, sql, &res))
        return -1;
    MYSQL_ROW row = mysql_fetch_row(res);
    int64_t uid = row && row[0] ? std::stoll(row[0]) : -1;
    mysql_free_result(res);
    return uid;
}

bool Database::ImportFromUsersJson(const std::string &json_path) {
    std::ifstream f(json_path);
    if (!f)
        return false;
    std::string line;
    int imported = 0;
    while (std::getline(f, line)) {
        if (line.empty())
            continue;
        auto pos = line.find(':');
        if (pos == std::string::npos)
            continue;
        std::string username = line.substr(0, pos);
        std::string encoded = line.substr(pos + 1);
        std::string decoded;
        static const char *t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        int val = 0, valb = -8;
        for (char c : encoded) {
            if (c == '=')
                break;
            const char *p = t;
            while (*p && *p != c)
                ++p;
            if (!*p)
                continue;
            val = (val << 6) + static_cast<int>(p - t);
            valb += 6;
            if (valb >= 0) {
                decoded += static_cast<char>((val >> valb) & 0xFF);
                valb -= 8;
            }
        }
        if (AddUser(username, sha256::hash_password(decoded)))
            imported++;
    }
    printf("[DB] Imported %d users from %s\n", imported, json_path.c_str());
    return imported > 0;
}

// ---- data helpers ----

static int countRows(const std::string &json) {
    if (json.empty() || json == "[]")
        return 0;
    int n = 1;
    for (size_t i = 0; i < json.size(); ++i)
        if (json[i] == ']' && i + 2 < json.size() && json[i + 1] == ',' && json[i + 2] == '[')
            n++;
    return n;
}
static int countCols(const std::string &json) {
    if (json.empty() || json == "[]")
        return 0;
    int n = 1;
    bool in_string = false;
    for (size_t i = 0; i < json.size(); ++i) {
        if (json[i] == '"' && (i == 0 || json[i - 1] != '\\'))
            in_string = !in_string;
        if (!in_string && json[i] == ',')
            n++;
    }
    return n;
}

// ---- Spreadsheets ----

bool Database::CreateSpreadsheet(int64_t user_id, const std::string &username, const std::string &name,
                                 const std::string &desc, const std::string &headers_json, const std::string &data_json,
                                 int64_t &out_id, const std::string &idempotency_key) {
    MYSQL *ec = EscConn();
    int64_t snowflake_id = snowflake_ ? snowflake_->Next() : 0;
    int rc = countRows(data_json), cc = countCols(headers_json);
    std::string cols =
        "id,user_id,username,name,description,headers_json,data_"
        "json,row_count,col_count";
    std::string vals = std::to_string(snowflake_id) + "," + std::to_string(user_id) + "," + q(ec, username) + "," +
                       q(ec, name) + "," + q(ec, desc) + "," + q(ec, headers_json) + "," + q(ec, data_json) + "," +
                       std::to_string(rc) + "," + std::to_string(cc);
    std::string sql;
    if (!idempotency_key.empty()) {
        cols += ",idempotency_key";
        vals += "," + q(ec, idempotency_key);
        sql = "INSERT INTO spreadsheets (" + cols + ") VALUES (" + vals +
              ")"
              " ON DUPLICATE KEY UPDATE id=LAST_INSERT_ID(id)";
    } else {
        sql = "INSERT INTO spreadsheets (" + cols + ") VALUES (" + vals + ")";
    }
    return ExecWriteInsert(sql, out_id);
}

bool Database::GetSpreadsheet(int64_t id, int64_t user_id, SpreadsheetRow &out) {
    if (user_id <= 0)
        return false;
    std::string sql =
        "SELECT id,username,name,description,headers_json,data_json,"
        "row_count,col_count,created_at,updated_at,version,storage_path FROM "
        "spreadsheets WHERE id=" +
        std::to_string(id) + " AND user_id=" + std::to_string(user_id);
    return ExecRead(sql, [&](MYSQL_RES *res) {
        MYSQL_ROW row = mysql_fetch_row(res);
        if (!row)
            return false;
        out.id = std::stoll(row[0]);
        out.username = row[1] ? row[1] : "";
        out.name = row[2] ? row[2] : "";
        out.description = row[3] ? row[3] : "";
        out.headers_json = row[4] ? row[4] : "";
        out.data_json = row[5] ? row[5] : "";
        out.row_count = row[6] ? std::stoi(row[6]) : 0;
        out.col_count = row[7] ? std::stoi(row[7]) : 0;
        out.created_at = row[8] ? row[8] : "";
        out.updated_at = row[9] ? row[9] : "";
        out.version = row[10] ? std::stoi(row[10]) : 1;
        out.storage_path = row[11] ? row[11] : "";
        return true;
    });
}

bool Database::ListSpreadsheets(int64_t user_id, std::vector<SpreadsheetSummary> &out, int &total, int page,
                                int page_size, int64_t after_id) {
    std::string where = "WHERE user_id=" + std::to_string(user_id);
    if (after_id > 0)
        where += " AND id<" + std::to_string(after_id);

    // Always obtain exact total via COUNT(*) — avoids loading all rows just to
    // count them
    total = 0;
    std::string count_sql = "SELECT COUNT(*) FROM spreadsheets " + where;
    ExecRead(count_sql, [&](MYSQL_RES *res) {
        MYSQL_ROW row = mysql_fetch_row(res);
        if (row && row[0])
            total = std::stoi(row[0]);
        return true;
    });

    std::string sql;
    if (page_size > 0) {
        int offset = page * page_size;
        // Delayed Join: subquery scans only (username, updated_at) covering index
        // to find ids, then join back to fetch full row data — avoids deep-page row
        // reads.
        sql =
            "SELECT s.id,s.name,s.description,s.row_count,s.col_count,s.updated_at "
            "FROM spreadsheets s "
            "JOIN (SELECT id FROM spreadsheets " +
            where +
            " ORDER BY updated_at DESC"
            " LIMIT " +
            std::to_string(page_size) + " OFFSET " + std::to_string(offset) +
            ") tmp ON s.id=tmp.id "
            "ORDER BY s.updated_at DESC";
    } else {
        sql =
            "SELECT id,name,description,row_count,col_count,updated_at "
            "FROM spreadsheets " +
            where + " ORDER BY updated_at DESC";
    }
    return ExecRead(sql, [&](MYSQL_RES *res) {
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(res))) {
            SpreadsheetSummary s;
            s.id = std::stoll(row[0]);
            s.name = row[1] ? row[1] : "";
            s.description = row[2] ? row[2] : "";
            s.row_count = row[3] ? std::stoi(row[3]) : 0;
            s.col_count = row[4] ? std::stoi(row[4]) : 0;
            s.updated_at = row[5] ? row[5] : "";
            out.push_back(s);
        }
        return true;
    });
}

bool Database::UpdateSpreadsheet(int64_t id, int64_t /*user_id*/, const std::string &name, const std::string &desc,
                                 const std::string &headers_json, const std::string &data_json, int version) {
    return UpdateSpreadsheet(id, name, desc, headers_json, data_json, version);
}

bool Database::UpdateSpreadsheet(int64_t id, const std::string &name, const std::string &desc,
                                 const std::string &headers_json, const std::string &data_json, int version) {
    MYSQL *ec = EscConn();
    int rc = countRows(data_json), cc = countCols(headers_json);
    std::string sql = "UPDATE spreadsheets SET name=" + q(ec, name) + ",description=" + q(ec, desc) +
                      ",headers_json=" + q(ec, headers_json) + ",data_json=" + q(ec, data_json) +
                      ",row_count=" + std::to_string(rc) + ",col_count=" + std::to_string(cc) +
                      ",version = version + 1, updated_at=NOW()" + " WHERE id=" + std::to_string(id);
    if (version > 0) {
        sql += " AND version = " + std::to_string(version);
    }
    // 乐观锁冲突时 affected_rows=0 → ExecWrite 返回 true(无 SQL 错误)
    // 业务层通过返回值无法区分"未匹配"和"成功", 需额外检查
    // 这里改为: version>0 时检查 affected_rows, 不匹配则返回 false
    if (version > 0) {
        // 走带版本号的更新, 需要检查受影响行数
        if (write_conns_.empty())
            return false;
        size_t idx = write_idx_.fetch_add(1, std::memory_order_relaxed) % write_conns_.size();
        auto &wc = write_conns_[idx];
        std::lock_guard<std::mutex> lock(wc->mtx);
        if (!wc->conn)
            return false;
        if (mysql_query(wc->conn, sql.c_str()) != 0) {
            fprintf(stderr, "[DB:write#%zu] UpdateSpreadsheet error: %s\n", idx, mysql_error(wc->conn));
            return false;
        }
        return mysql_affected_rows(wc->conn) > 0;
    }
    return ExecWrite(sql);
}

bool Database::DeleteSpreadsheet(int64_t id, int64_t /*user_id*/) {
    return ExecWrite("DELETE FROM spreadsheets WHERE id=" + std::to_string(id));
}

bool Database::GetSpreadsheetOwner(int64_t id, int64_t &owner_user_id, int *out_version) {
    // 强一致: 归属校验必须读主库
    if (write_conns_.empty())
        return false;
    size_t idx = write_idx_.fetch_add(1, std::memory_order_relaxed) % write_conns_.size();
    auto &wc = write_conns_[idx];
    std::lock_guard<std::mutex> lock(wc->mtx);
    if (!wc->conn)
        return false;
    std::string sql = "SELECT user_id, version FROM spreadsheets WHERE id=" + std::to_string(id);
    MYSQL_RES *res = nullptr;
    if (!RunQuery(wc->conn, sql, &res))
        return false;
    MYSQL_ROW row = mysql_fetch_row(res);
    if (row) {
        owner_user_id = row[0] ? std::stoll(row[0]) : 0;
        if (out_version)
            *out_version = row[1] ? std::stoi(row[1]) : 1;
    }
    mysql_free_result(res);
    return row != nullptr;
}

// ---- Files ----

bool Database::CreateFile(int64_t user_id, const std::string &username, const std::string &original_name, int64_t size,
                          const std::string &mime_type, const std::string &storage_key, int64_t &out_id,
                          const std::string &idempotency_key) {
    MYSQL *ec = EscConn();
    int64_t snowflake_id = snowflake_ ? snowflake_->Next() : 0;
    // File body lives in object storage (MinIO); only metadata goes into MySQL.
    // storage_path holds the MinIO object key; file_content column is left NULL.
    std::string cols = "id,user_id,username,original_name,size,mime_type";
    std::string vals = std::to_string(snowflake_id) + "," + std::to_string(user_id) + "," + q(ec, username) + "," +
                       q(ec, original_name) + "," + std::to_string(size) + "," + q(ec, mime_type);
    if (!storage_key.empty()) {
        cols += ",storage_path";
        vals += "," + q(ec, storage_key);
    }
    std::string sql;
    if (!idempotency_key.empty()) {
        cols += ",idempotency_key";
        vals += "," + q(ec, idempotency_key);
        sql = "INSERT INTO files (" + cols + ") VALUES (" + vals +
              ")"
              " ON DUPLICATE KEY UPDATE id=LAST_INSERT_ID(id)";
    } else {
        sql = "INSERT INTO files (" + cols + ") VALUES (" + vals + ")";
    }
    return ExecWriteInsert(sql, out_id);
}

bool Database::UpdateFileContent(int64_t id, const std::string &content) {
    if (write_conns_.empty())
        return false;
    size_t idx = write_idx_.fetch_add(1, std::memory_order_relaxed) % write_conns_.size();
    auto &wc = write_conns_[idx];
    std::lock_guard<std::mutex> lock(wc->mtx);
    if (!wc->conn)
        return false;
    char *esc = (char *)malloc(content.size() * 2 + 1);
    mysql_real_escape_string(wc->conn, esc, content.data(), (unsigned long)content.size());
    std::string sql = "UPDATE files SET file_content='" + std::string(esc) + "' WHERE id=" + std::to_string(id);
    free(esc);
    return mysql_query(wc->conn, sql.c_str()) == 0;
}

bool Database::GetFile(int64_t id, int64_t user_id, FileRow &out) {
    if (user_id <= 0)
        return false;
    // storage_path is col 7: if non-empty, caller should fetch content from
    // object storage instead of reading the legacy file_content LONGBLOB (col 6)
    std::string sql =
        "SELECT id,username,original_name,size,mime_type,created_at,"
        "file_content,storage_path FROM files WHERE id=" +
        std::to_string(id) + " AND user_id=" + std::to_string(user_id);
    return ExecRead(sql, [&](MYSQL_RES *res) {
        MYSQL_ROW row = mysql_fetch_row(res);
        if (!row)
            return false;
        out.id = std::stoll(row[0]);
        out.username = row[1] ? row[1] : "";
        out.original_name = row[2] ? row[2] : "";
        out.size = row[3] ? std::stoll(row[3]) : 0;
        out.mime_type = row[4] ? row[4] : "";
        out.created_at = row[5] ? row[5] : "";
        out.storage_path = row[7] ? row[7] : "";
        if (out.storage_path.empty() && row[6]) {
            unsigned long *lens = mysql_fetch_lengths(res);
            if (lens)
                out.file_content.assign(row[6], lens[6]);
        }
        return true;
    });
}

bool Database::ListFiles(int64_t user_id, std::vector<FileRow> &out, int &total, int page, int page_size,
                         int64_t after_id) {
    std::string where = "WHERE user_id=" + std::to_string(user_id);
    if (after_id > 0)
        where += " AND id<" + std::to_string(after_id);

    // Always obtain exact total via COUNT(*) to avoid loading all rows just to
    // count
    total = 0;
    std::string count_sql = "SELECT COUNT(*) FROM files " + where;
    ExecRead(count_sql, [&](MYSQL_RES *res) {
        MYSQL_ROW row = mysql_fetch_row(res);
        if (row && row[0])
            total = std::stoi(row[0]);
        return true;
    });

    std::string sql;
    if (page_size > 0) {
        int offset = page * page_size;
        // Delayed Join: subquery uses covering index (username, created_at) for id
        // lookup, then join back to read full row — avoids scanning large row data
        // at deep offsets.
        sql =
            "SELECT "
            "f.id,f.username,f.original_name,f.size,f.mime_type,f.created_at "
            "FROM files f "
            "JOIN (SELECT id FROM files " +
            where +
            " ORDER BY created_at DESC"
            " LIMIT " +
            std::to_string(page_size) + " OFFSET " + std::to_string(offset) +
            ") tmp ON f.id=tmp.id "
            "ORDER BY f.created_at DESC";
    } else {
        sql =
            "SELECT id,username,original_name,size,mime_type,created_at "
            "FROM files " +
            where + " ORDER BY created_at DESC";
    }
    return ExecRead(sql, [&](MYSQL_RES *res) {
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(res))) {
            FileRow f;
            f.id = std::stoll(row[0]);
            f.username = row[1] ? row[1] : "";
            f.original_name = row[2] ? row[2] : "";
            f.size = row[3] ? std::stoll(row[3]) : 0;
            f.mime_type = row[4] ? row[4] : "";
            f.created_at = row[5] ? row[5] : "";
            out.push_back(f);
        }
        return true;
    });
}

bool Database::DeleteFile(int64_t id, int64_t /*user_id*/) {
    return ExecWrite("DELETE FROM files WHERE id=" + std::to_string(id));
}

bool Database::GetFileOwner(int64_t id, int64_t &owner_user_id) {
    // 强一致: 归属校验必须读主库
    if (write_conns_.empty())
        return false;
    size_t idx = write_idx_.fetch_add(1, std::memory_order_relaxed) % write_conns_.size();
    auto &wc = write_conns_[idx];
    std::lock_guard<std::mutex> lock(wc->mtx);
    if (!wc->conn)
        return false;
    std::string sql = "SELECT user_id FROM files WHERE id=" + std::to_string(id);
    MYSQL_RES *res = nullptr;
    if (!RunQuery(wc->conn, sql, &res))
        return false;
    MYSQL_ROW row = mysql_fetch_row(res);
    if (row)
        owner_user_id = row[0] ? std::stoll(row[0]) : 0;
    mysql_free_result(res);
    return row != nullptr;
}

bool Database::GetFileStoragePath(int64_t id, std::string &storage_path) {
    // 查询 MinIO object key，用于删除时一并清理对象存储
    storage_path.clear();
    std::string sql = "SELECT storage_path FROM files WHERE id=" + std::to_string(id);
    return ExecRead(sql, [&](MYSQL_RES *res) {
        MYSQL_ROW row = mysql_fetch_row(res);
        if (row && row[0])
            storage_path = row[0];
        return row != nullptr;
    });
}

bool Database::GetSpreadsheetStoragePath(int64_t id, std::string &storage_path) {
    storage_path.clear();
    std::string sql = "SELECT storage_path FROM spreadsheets WHERE id=" + std::to_string(id);
    return ExecRead(sql, [&](MYSQL_RES *res) {
        MYSQL_ROW row = mysql_fetch_row(res);
        if (row && row[0])
            storage_path = row[0];
        return row != nullptr;
    });
}
bool Database::UpdateSpreadsheetStoragePath(int64_t id, const std::string &storage_path) {
    auto *ec = EscConn();
    if (!ec)
        return false;
    std::string sql = "UPDATE spreadsheets SET storage_path=" + q(ec, storage_path) + " WHERE id=" + std::to_string(id);
    return ExecWrite(sql);
}

// ---- Undo Log (always master — 2PC requires consistency) ----

bool Database::WriteUndoLog(const std::string &xid, const std::string &table_name, int64_t row_id,
                            const std::string &before_snapshot) {
    MYSQL *ec = EscConn();
    std::string sql = "INSERT INTO undo_log (xid,table_name,row_id,before_snapshot) VALUES (" + q(ec, xid) + "," +
                      q(ec, table_name) + "," + std::to_string(row_id) + "," + q(ec, before_snapshot) + ")";
    return ExecWrite(sql);
}

bool Database::GetUndoLog(const std::string &xid, std::string &table_name, int64_t &row_id,
                          std::string &before_snapshot) {
    // 2PC 一致性: undo_log 必须读主库
    if (write_conns_.empty())
        return false;
    size_t idx = write_idx_.fetch_add(1, std::memory_order_relaxed) % write_conns_.size();
    auto &wc = write_conns_[idx];
    std::lock_guard<std::mutex> lock(wc->mtx);
    if (!wc->conn)
        return false;
    std::string sql =
        "SELECT table_name,row_id,before_snapshot FROM undo_log "
        "WHERE xid=" +
        q(wc->conn, xid) + " ORDER BY id DESC LIMIT 1";
    MYSQL_RES *res = nullptr;
    if (!RunQuery(wc->conn, sql, &res))
        return false;
    MYSQL_ROW row = mysql_fetch_row(res);
    if (!row) {
        mysql_free_result(res);
        return false;
    }
    table_name = row[0] ? row[0] : "";
    row_id = row[1] ? std::stoll(row[1]) : 0;
    before_snapshot = row[2] ? row[2] : "";
    mysql_free_result(res);
    return true;
}

bool Database::ClearUndoLog(const std::string &xid) {
    MYSQL *ec = EscConn();
    return ExecWrite("DELETE FROM undo_log WHERE xid=" + q(ec, xid));
}

int Database::PurgeOldUndoLogs(int days) {
    std::string sql = "DELETE FROM undo_log WHERE created_at < NOW() - INTERVAL " + std::to_string(days) + " DAY";
    if (write_conns_.empty())
        return -1;
    size_t idx = write_idx_.fetch_add(1, std::memory_order_relaxed) % write_conns_.size();
    auto &wc = write_conns_[idx];
    std::lock_guard<std::mutex> lock(wc->mtx);
    if (!wc->conn)
        return -1;
    if (mysql_query(wc->conn, sql.c_str()) != 0) {
        fprintf(stderr, "[DB] PurgeOldUndoLogs error: %s\n", mysql_error(wc->conn));
        return -1;
    }
    int affected = (int)mysql_affected_rows(wc->conn);
    printf("[DB] PurgeOldUndoLogs: removed %d rows older than %d days\n", affected, days);
    return affected;
}

// ---- 连接池健康检查 ----

void Database::StartHealthCheck() {
    health_running_ = true;
    health_check_ = std::thread(&Database::HealthLoop, this);
    printf("[DB] Health check started (every 30s)\n");
}

void Database::StopHealthCheck() {
    health_running_ = false;
    if (health_check_.joinable())
        health_check_.join();
}

void Database::HealthLoop() {
    while (health_running_) {
        std::this_thread::sleep_for(std::chrono::seconds(30));
        if (!health_running_)
            break;

        for (size_t i = 0; i < write_conns_.size(); ++i) {
            auto &wc = write_conns_[i];
            std::lock_guard<std::mutex> lock(wc->mtx);
            if (wc->conn && mysql_ping(wc->conn) != 0) {
                fprintf(stderr, "[DB:health] write conn %zu dead, reconnecting\n", i);
                mysql_close(wc->conn);
                wc->conn = ConnectMYSQL(write_host_, write_port_);
            }
        }

        for (size_t i = 0; i < read_conns_.size(); ++i) {
            auto &rc = read_conns_[i];
            std::lock_guard<std::mutex> lock(rc->mtx);
            if (rc->conn && mysql_ping(rc->conn) != 0) {
                fprintf(stderr, "[DB:health] read conn %zu dead, reconnecting\n", i);
                mysql_close(rc->conn);
                rc->conn = ConnectMYSQL(read_hosts_[i % read_hosts_.size()], read_port_);
            }
        }
    }
}

// ============================================================
// ShardedDatabase — hash-routes user_id across N Database shards
// ============================================================

ShardedDatabase::ShardedDatabase(int shard_count, const std::string &write_host_prefix, int write_port,
                                 const std::string &read_hosts_prefix, int read_port, const std::string &user,
                                 const std::string &password, const std::string &db_name_prefix, int write_pool_size)
    : shard_count_(shard_count),
      write_host_prefix_(write_host_prefix),
      read_hosts_prefix_(read_hosts_prefix),
      db_name_prefix_(db_name_prefix),
      write_port_(write_port),
      read_port_(read_port),
      user_(user),
      password_(password),
      write_pool_size_(write_pool_size) {
    for (int i = 0; i < shard_count; i++) {
        // shard_count==1: use host/db name as-is (no suffix), backward compat
        std::string wh = (shard_count == 1) ? write_host_prefix : write_host_prefix + "-" + std::to_string(i);
        std::string rh = (shard_count == 1) ? read_hosts_prefix : read_hosts_prefix + "-" + std::to_string(i);
        std::string db_name = (shard_count == 1) ? db_name_prefix : db_name_prefix + "_" + std::to_string(i);
        auto db = std::make_unique<Database>(wh, write_port, rh, read_port, user, password, db_name, write_pool_size);
        shards_.push_back(std::move(db));
    }
    printf("[ShardedDB] %d shards created (prefix: %s)\n", shard_count, write_host_prefix.c_str());
}

void ShardedDatabase::InitializeAll() {
    for (auto &db : shards_) {
        for (int retry = 0; retry < 30; ++retry) {
            if (db->Initialize()) break;
            if (retry == 0) fprintf(stderr, "[DB] Waiting for MySQL to be ready...\n");
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    }
}

Database *ShardedDatabase::ShardFor(int64_t user_id) {
    int idx = static_cast<int>(std::abs(user_id) % shard_count_);
    return shards_[idx].get();
}

Database *ShardedDatabase::ShardForBroadcast() {
    return shards_.empty() ? nullptr : shards_[0].get();
}

// === Users (auth shard only — shard 0) ===

bool ShardedDatabase::AddUser(const std::string &username, const std::string &password_hash) {
    return shards_[0]->AddUser(username, password_hash);
}
bool ShardedDatabase::GetUser(const std::string &username, std::string &password_hash) {
    return shards_[0]->GetUser(username, password_hash);
}
bool ShardedDatabase::UserExists(const std::string &username) {
    return shards_[0]->UserExists(username);
}
int ShardedDatabase::GetTokenVersion(const std::string &username) {
    return shards_[0]->GetTokenVersion(username);
}
bool ShardedDatabase::IncrementTokenVersion(const std::string &username) {
    return shards_[0]->IncrementTokenVersion(username);
}
int64_t ShardedDatabase::GetUserId(const std::string &username) {
    return shards_[0]->GetUserId(username);
}
bool ShardedDatabase::ImportFromUsersJson(const std::string &json_path) {
    return shards_[0]->ImportFromUsersJson(json_path);
}

// === Spreadsheets (hash by user_id) ===

bool ShardedDatabase::CreateSpreadsheet(int64_t user_id, const std::string &username, const std::string &name,
                                        const std::string &desc, const std::string &headers_json,
                                        const std::string &data_json, int64_t &out_id,
                                        const std::string &idempotency_key) {
    return ShardFor(user_id)->CreateSpreadsheet(user_id, username, name, desc, headers_json, data_json, out_id,
                                                idempotency_key);
}
bool ShardedDatabase::GetSpreadsheet(int64_t id, int64_t user_id, SpreadsheetRow &out) {
    return ShardFor(user_id)->GetSpreadsheet(id, user_id, out);
}
bool ShardedDatabase::ListSpreadsheets(int64_t user_id, std::vector<SpreadsheetSummary> &out, int &total, int page,
                                       int page_size, int64_t after_id) {
    return ShardFor(user_id)->ListSpreadsheets(user_id, out, total, page, page_size, after_id);
}
bool ShardedDatabase::UpdateSpreadsheet(int64_t id, int64_t user_id, const std::string &name, const std::string &desc,
                                        const std::string &headers_json, const std::string &data_json, int version) {
    return ShardFor(user_id)->UpdateSpreadsheet(id, name, desc, headers_json, data_json, version);
}
bool ShardedDatabase::UpdateSpreadsheet(int64_t id, const std::string &name, const std::string &desc,
                                        const std::string &headers_json, const std::string &data_json, int version) {
    // Legacy: no user_id → broadcast
    for (auto &db : shards_)
        if (db->UpdateSpreadsheet(id, name, desc, headers_json, data_json, version))
            return true;
    return false;
}

// Broadcast: id is globally unique, try each shard
bool ShardedDatabase::DeleteSpreadsheet(int64_t id, int64_t user_id) {
    if (user_id > 0)
        return ShardFor(user_id)->DeleteSpreadsheet(id);
    for (auto &db : shards_)
        if (db->DeleteSpreadsheet(id))
            return true;
    return false;
}

bool ShardedDatabase::DeleteFile(int64_t id, int64_t user_id) {
    if (user_id > 0)
        return ShardFor(user_id)->DeleteFile(id);
    for (auto &db : shards_)
        if (db->DeleteFile(id))
            return true;
    return false;
}

// Broadcast: id is globally unique, try each shard
bool ShardedDatabase::GetSpreadsheetOwner(int64_t id, int64_t &owner_user_id, int *out_version) {
    for (auto &db : shards_)
        if (db->GetSpreadsheetOwner(id, owner_user_id, out_version))
            return true;
    return false;
}

// === Files (hash by user_id) ===

bool ShardedDatabase::CreateFile(int64_t user_id, const std::string &username, const std::string &original_name,
                                 int64_t size, const std::string &mime_type, const std::string &storage_key,
                                 int64_t &out_id, const std::string &idempotency_key) {
    return ShardFor(user_id)->CreateFile(user_id, username, original_name, size, mime_type, storage_key, out_id,
                                         idempotency_key);
}
bool ShardedDatabase::UpdateFileContent(int64_t id, const std::string &content) {
    return ShardFor(id)->UpdateFileContent(id, content);
}
bool ShardedDatabase::GetFile(int64_t id, int64_t user_id, FileRow &out) {
    return ShardFor(user_id)->GetFile(id, user_id, out);
}
bool ShardedDatabase::ListFiles(int64_t user_id, std::vector<FileRow> &out, int &total, int page, int page_size,
                                int64_t after_id) {
    return ShardFor(user_id)->ListFiles(user_id, out, total, page, page_size, after_id);
}

// Broadcast: id is globally unique
bool ShardedDatabase::GetFileOwner(int64_t id, int64_t &owner_user_id) {
    for (auto &db : shards_)
        if (db->GetFileOwner(id, owner_user_id))
            return true;
    return false;
}
bool ShardedDatabase::GetFileStoragePath(int64_t id, std::string &storage_path) {
    for (auto &db : shards_)
        if (db->GetFileStoragePath(id, storage_path))
            return true;
    return false;
}
// ---- Outbox ----
bool Database::InsertOutbox(const std::string &event_type, const std::string &payload,
                             const std::string &trace_context) {
    auto *ec = EscConn();
    if (!ec)
        return false;
    std::string cols = "event_type, payload";
    std::string vals = q(ec, event_type) + "," + q(ec, payload);
    if (!trace_context.empty()) {
        cols += ", trace_context";
        vals += "," + q(ec, trace_context);
    }
    return ExecWrite("INSERT INTO outbox (" + cols + ") VALUES (" + vals + ")");
}
bool ShardedDatabase::InsertOutbox(int64_t user_id, const std::string &event_type, const std::string &payload,
                                   const std::string &trace_context) {
    return ShardFor(user_id)->InsertOutbox(event_type, payload, trace_context);
}
bool ShardedDatabase::GetSpreadsheetStoragePath(int64_t id, std::string &storage_path) {
    for (auto &db : shards_)
        if (db->GetSpreadsheetStoragePath(id, storage_path))
            return true;
    return false;
}
bool ShardedDatabase::UpdateSpreadsheetStoragePath(int64_t id, const std::string &storage_path) {
    for (auto &db : shards_)
        if (db->UpdateSpreadsheetStoragePath(id, storage_path))
            return true;
    return false;
}

// === Undo Log (broadcast — 2PC 一致性要求) ===

bool ShardedDatabase::WriteUndoLog(const std::string &xid, const std::string &table_name, int64_t row_id,
                                   const std::string &before_snapshot) {
    for (auto &db : shards_)
        db->WriteUndoLog(xid, table_name, row_id, before_snapshot);
    return true;
}
bool ShardedDatabase::GetUndoLog(const std::string &xid, std::string &table_name, int64_t &row_id,
                                 std::string &before_snapshot) {
    for (auto &db : shards_)
        if (db->GetUndoLog(xid, table_name, row_id, before_snapshot))
            return true;
    return false;
}
bool ShardedDatabase::ClearUndoLog(const std::string &xid) {
    for (auto &db : shards_)
        db->ClearUndoLog(xid);
    return true;
}
int ShardedDatabase::PurgeOldUndoLogs(int days) {
    int total = 0;
    for (auto &db : shards_)
        total += db->PurgeOldUndoLogs(days);
    return total;
}

// ---- Account management helpers ----
int64_t Database::GetUserIdByPhone(const std::string &phone) {
    std::string sql = "SELECT id FROM users WHERE phone=" + q(EscConn(), phone);
    int64_t uid = -1;
    ExecRead(sql, [&](MYSQL_RES *res) {
        MYSQL_ROW row = mysql_fetch_row(res);
        if (row && row[0])
            uid = std::stoll(row[0]);
        return true;
    });
    return uid;
}
std::string Database::GetUsernameById(int64_t user_id) {
    std::string sql = "SELECT username FROM users WHERE id=" + std::to_string(user_id);
    std::string name;
    ExecRead(sql, [&](MYSQL_RES *res) {
        MYSQL_ROW row = mysql_fetch_row(res);
        if (row && row[0])
            name = row[0];
        return true;
    });
    return name;
}
bool Database::VerifyUserPassword(int64_t user_id, const std::string &password) {
    std::string sql = "SELECT password_hash FROM users WHERE id=" + std::to_string(user_id);
    std::string stored;
    ExecRead(sql, [&](MYSQL_RES *res) {
        MYSQL_ROW row = mysql_fetch_row(res);
        if (row && row[0])
            stored = row[0];
        return true;
    });
    return sha256::verify_password(password, stored);
}
bool Database::UpdateUserPassword(int64_t user_id, const std::string &new_hash) {
    auto *ec = EscConn();
    std::string sql = "UPDATE users SET password_hash=" + q(ec, new_hash) + " WHERE id=" + std::to_string(user_id);
    return ExecWrite(sql);
}
int64_t ShardedDatabase::GetUserIdByPhone(const std::string &phone) {
    for (auto &db : shards_) {
        int64_t uid = db->GetUserIdByPhone(phone);
        if (uid > 0)
            return uid;
    }
    return -1;
}
std::string ShardedDatabase::GetUsernameById(int64_t user_id) {
    for (auto &db : shards_) {
        auto name = db->GetUsernameById(user_id);
        if (!name.empty())
            return name;
    }
    return "";
}
bool ShardedDatabase::VerifyUserPassword(int64_t user_id, const std::string &pwd) {
    for (auto &db : shards_)
        if (db->VerifyUserPassword(user_id, pwd))
            return true;
    return false;
}
bool ShardedDatabase::UpdateUserPassword(int64_t user_id, const std::string &h) {
    for (auto &db : shards_)
        if (db->UpdateUserPassword(user_id, h))
            return true;
    return false;
}

// ---- Folder operations ----
bool Database::CreateFolder(int64_t user_id, const std::string &name, int64_t parent_id, int64_t &out_id) {
    auto *ec = EscConn();
    if (!ec)
        return false;
    std::string sql =
        "INSERT INTO files (user_id,username,original_name,size,mime_type,folder_id,is_folder,file_content) VALUES (" +
        std::to_string(user_id) + ",'folder'," + q(ec, name) + ",0,''," + std::to_string(parent_id) + ",1,'')";
    return ExecWriteInsert(sql, out_id);
}
bool Database::MoveFile(int64_t id, int64_t target_folder_id) {
    std::string sql =
        "UPDATE files SET folder_id=" + std::to_string(target_folder_id) + " WHERE id=" + std::to_string(id);
    return ExecWrite(sql);
}
int Database::BatchDeleteFiles(int64_t user_id, const std::vector<int64_t> &ids) {
    int count = 0;
    for (auto id : ids) {
        std::string sql =
            "DELETE FROM files WHERE id=" + std::to_string(id) + " AND user_id=" + std::to_string(user_id);
        if (ExecWrite(sql))
            count++;
    }
    return count;
}
bool ShardedDatabase::CreateFolder(int64_t user_id, const std::string &name, int64_t parent_id, int64_t &out_id) {
    return ShardFor(user_id)->CreateFolder(user_id, name, parent_id, out_id);
}
bool ShardedDatabase::MoveFile(int64_t id, int64_t target_folder_id) {
    for (auto &db : shards_)
        if (db->MoveFile(id, target_folder_id))
            return true;
    return false;
}
int ShardedDatabase::BatchDeleteFiles(int64_t user_id, const std::vector<int64_t> &ids) {
    return ShardFor(user_id)->BatchDeleteFiles(user_id, ids);
}

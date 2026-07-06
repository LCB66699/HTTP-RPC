#include "shared/helper/json_helpers.h"
#include "shared/client/database.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include "shared/base/sha256.h"
#include "shared/base/snowflake.h"

// ---- connection helpers ----

MysqlPtr Database::ConnectMYSQL(const std::string &host, int port) {
    MysqlPtr c(mysql_init(nullptr));
    if (!c) {
        fprintf(stderr, "[DB] mysql_init failed\n");
        return nullptr;
    }
    // 閲嶈瘯 5 娆″簲锟?Docker DNS 闂存瓏鎬цВ鏋愬け锟?(CR_CONN_HOST_ERROR / ER_BAD_HOST_ERROR)
    for (int attempt = 1; attempt <= 5; attempt++) {
        if (mysql_real_connect(c.get(), host.c_str(), user_.c_str(), password_.c_str(),
                               db_name_.c_str(), port, nullptr, 0)) {
            mysql_autocommit(c.get(), 1);
            return c;
        }
        unsigned int err = mysql_errno(c.get());
        if (attempt < 5) {
            fprintf(stderr, "[DB] connect %s:%d attempt %d/5 failed (err=%u): %s\n",
                    host.c_str(), port, attempt, err, mysql_error(c.get()));
            std::this_thread::sleep_for(std::chrono::milliseconds(500 * attempt));
        }
    }
    fprintf(stderr, "[DB] connect %s:%d failed after 5 attempts: %s\n",
            host.c_str(), port, mysql_error(c.get()));
    // MysqlPtr 鏋愭瀯鑷姩 mysql_close
    return nullptr;
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

// ---- sql_param 鏋勯€犲嚱锟?锟?寮哄埗杞箟锛岀紪璇戞湡鍏滃簳 ----
sql_param::sql_param(MYSQL *conn, const std::string &s) : quote_(true) {
    if (!conn) {
        val_ = s;
        return;
    }
    val_.resize(s.size() * 2 + 1, '\0');
    unsigned long len = mysql_real_escape_string(conn, &val_[0], s.c_str(), s.size());
    val_.resize(len);
}

// 鍙栦换鎰忓彲鐢ㄨ繛鎺ヤ緵杞箟鐢紙閬嶅巻鍐欐睜鈫掕锟? 杩斿洖棣栦釜瀛樻椿杩炴帴锟?
MYSQL *Database::EscConn() {
    for (auto &wc : write_conns_)
        if (wc->conn)
            return wc->conn.get();
    for (auto &rc : read_conns_)
        if (rc->conn)
            return rc->conn.get();
    return nullptr;
}

Database::ConnHandle Database::GetHealthyWriteConn() {
    if (write_conns_.empty())
        return {std::unique_lock<std::mutex>{}, nullptr};
    size_t pool_sz = write_conns_.size();
    for (size_t attempt = 0; attempt < pool_sz; ++attempt) {
        size_t idx = write_idx_.fetch_add(1, std::memory_order_relaxed) % pool_sz;
        auto &wc = write_conns_[idx];
        {   // 姣忔潯杩炴帴鐙珛鍔犻攣, 澶辫触鍒欓噴鏀鹃攣缁х画璇曚笅涓€锟?
            std::unique_lock<std::mutex> lock(wc->mtx);
            if (!wc->conn) {
                wc->conn = ConnectMYSQL(write_host_, write_port_);
            } else {
                int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                if (now - wc->last_used_ms.load() > 5000) {
                    if (mysql_ping(wc->conn.get()) != 0) {
                        fprintf(stderr, "[DB:write#%zu] ping failed, reconnecting\n", idx);
                        wc->conn.reset();
                        wc->conn = ConnectMYSQL(write_host_, write_port_);
                    }
                }
            }
            if (wc->conn) {
                wc->last_used_ms.store(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count());
                return {std::move(lock), wc->conn.get()};
            }
        }   // lock released, try next connection
    }
    return {std::unique_lock<std::mutex>{}, nullptr};
}

Database::ConnHandle Database::GetHealthyReadConn() {
    if (!read_conns_.empty()) {
        size_t pool_sz = read_conns_.size();
        for (size_t attempt = 0; attempt < pool_sz; ++attempt) {
            size_t idx = read_idx_.fetch_add(1, std::memory_order_relaxed) % pool_sz;
            auto &rc = read_conns_[idx];
            {
                std::unique_lock<std::mutex> lock(rc->mtx);
                if (!rc->conn) {
                    rc->conn = ConnectMYSQL(read_hosts_[idx % read_hosts_.size()], read_port_);
                } else {
                    int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    if (now - rc->last_used_ms.load() > 5000) {
                        if (mysql_ping(rc->conn.get()) != 0) {
                            fprintf(stderr, "[DB:read#%zu] ping failed, reconnecting\n", idx);
                            rc->conn.reset();
                            rc->conn = ConnectMYSQL(read_hosts_[idx % read_hosts_.size()], read_port_);
                        }
                    }
                }
                if (rc->conn) {
                    rc->last_used_ms.store(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count());
                    return {std::move(lock), rc->conn.get()};
                }
            }
        }
    }
    // 鏃犱粠锟?锟?鎵€鏈夎杩炴帴鍧囦笉鍙敤, 鍥為€€鍒颁富锟?
    return GetHealthyWriteConn();
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
    // 鎷嗗垎璇诲湴鍧€
    std::string hosts = read_hosts;
    size_t start = 0, end;
    while ((end = hosts.find(',', start)) != std::string::npos) {
        read_hosts_.push_back(hosts.substr(start, end - start));
        start = end + 1;
    }
    read_hosts_.push_back(hosts.substr(start));

    // 鍒涘缓鍐欒繛鎺ユ睜 锟?鍚屼竴涓诲簱澶氭潯杩炴帴, 瀹炵幇骞惰锟?
    write_conns_.reserve(pool_size_);
    for (int i = 0; i < pool_size_; i++) {
        auto wc = std::make_unique<PoolConn>();
        wc->conn = ConnectMYSQL(write_host_, write_port_);
        if (!wc->conn)
            fprintf(stderr, "[DB] WARNING: write pool conn %d/%d failed\n", i + 1, pool_size_);
        write_conns_.push_back(std::move(wc));
    }

    // 鍒涘缓璇昏繛鎺ユ睜锛堜粠搴擄級
    for (auto &h : read_hosts_) {
        auto c = ConnectMYSQL(h, read_port_);
        if (c) {
            auto rc = std::make_unique<PoolConn>();
            rc->conn = std::move(c);
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
    // MysqlPtr 鏋愭瀯鑷姩 mysql_close锛屾棤闇€鎵嬪姩閬嶅巻
}

// ---- 鍐欐搷浣滐紙涓诲簱杩炴帴锟?round-robin 鍒嗗彂, 姣忚繛鎺ョ嫭锟?mutex锟?----

bool Database::ExecWrite(const std::string &sql) {
    auto h = GetHealthyWriteConn();
    if (!h)
        return false;
    if (mysql_query(h.conn, sql.c_str()) == 0)
        return true;
    fprintf(stderr, "[DB:write] error %u: %s\n", mysql_errno(h.conn), mysql_error(h.conn));
    return false;
}

// 锟?+ 杩斿洖鑷ID: INSERT 锟?mysql_insert_id 鍦ㄥ悓涓€杩炴帴鍚屼竴閿佸唴鍘熷瓙瀹屾垚
bool Database::ExecWriteInsert(const std::string &sql, int64_t &out_id) {
    auto h = GetHealthyWriteConn();
    if (!h)
        return false;
    if (mysql_query(h.conn, sql.c_str()) != 0) {
        fprintf(stderr, "[DB:write] ExecWriteInsert error: %s\n", mysql_error(h.conn));
        return false;
    }
    out_id = mysql_insert_id(h.conn);
    return true;
}

// ---- 璇绘搷浣滐紙浠庡簱杞, 鏃犱粠搴撴椂鍥為€€鍒板啓姹狅級 ----

MYSQL *Database::GetReadConn() {
    if (read_conns_.empty()) {
        for (auto &wc : write_conns_)
            if (wc->conn)
                return wc->conn.get();
        return nullptr;
    }
    size_t idx = read_idx_.fetch_add(1, std::memory_order_relaxed) % read_conns_.size();
    return read_conns_[idx]->conn.get();
}

bool Database::ExecRead(const std::string &sql, std::function<bool(MYSQL_RES *)> handler) {
    auto h = GetHealthyReadConn();
    if (!h)
        return false;
    MYSQL_RES *res = nullptr;
    if (!RunQuery(h.conn, sql, &res))
        return false;
    bool ok = handler(res);
    if (res)
        mysql_free_result(res);
    return ok;
}

// ---- schema init锛堥杩炴帴鎵ц, DDL 鍏ㄥ眬鐢熸晥鏃犻渶鍦ㄦ墍鏈夎繛鎺ヤ笂閲嶅锟?----

bool Database::Initialize() {
    if (write_conns_.empty())
        return false;

    // 鍚姩鏃跺簭闂锛氫笉绠¤繛鎺ユ槸锟?null锛屽叏閮ㄩ噸寤虹‘淇濊繛鍒板悓涓€涓氨缁殑 MySQL
    for (size_t i = 0; i < write_conns_.size(); ++i) {
        write_conns_[i]->conn = ConnectMYSQL(write_host_, write_port_);
        if (!write_conns_[i]->conn)
            return false;
    }
    for (size_t i = 0; i < read_conns_.size(); ++i) {
        read_conns_[i]->conn = ConnectMYSQL(read_hosts_[i % read_hosts_.size()], read_port_);
    }

    // 姣忔潯鍐欒繛鎺ラ兘鎵ц DDL锛屾潨缁濆洜杩炴帴鐘舵€佸樊寮傚鑷寸殑 Table not found
    for (auto &wcp : write_conns_) {
        if (!wcp->conn) continue;
        std::lock_guard<std::mutex> lock(wcp->mtx);
        MYSQL *c = wcp->conn.get();

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

    // Outbox 状态字段: 0=pending 1=claimed 2=published 255=dead
    exec("ALTER TABLE outbox ADD COLUMN status TINYINT NOT NULL DEFAULT 0");
    exec("ALTER TABLE outbox ADD COLUMN attempts TINYINT NOT NULL DEFAULT 0");
    exec("ALTER TABLE outbox ADD COLUMN published_at DATETIME DEFAULT NULL");
    exec("ALTER TABLE outbox ADD COLUMN last_error VARCHAR(512) DEFAULT NULL");
    exec("ALTER TABLE outbox ADD COLUMN message_id VARCHAR(64) DEFAULT NULL");

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

    // Idempotency key columns 锟?NULL means "no deduplication" (multiple NULLs
    // allowed by UNIQUE). ON DUPLICATE KEY UPDATE id=LAST_INSERT_ID(id) returns
    // the existing id on replay.
    mysql_query(c,
                "ALTER TABLE spreadsheets ADD COLUMN idempotency_key CHAR(36) "
                "UNIQUE NULL DEFAULT NULL");
    mysql_query(c,
                "ALTER TABLE files        ADD COLUMN idempotency_key CHAR(36) "
                "UNIQUE NULL DEFAULT NULL");

    // Version columns for optimistic locking 锟?spreadsheets already has it;
    // files needs it for multi-instance write safety.
    mysql_query(c, "ALTER TABLE files ADD COLUMN version INT NOT NULL DEFAULT 1");

    // user_id columns 锟?integer FK to users.id; avoids username-as-FK design
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
    // sort column. DROP first (idempotent 锟?fails silently if already removed),
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

    // 瑕嗙洊绱㈠紩 锟?SELECT token_version FROM users WHERE username=X 閬垮厤鍥炶〃
    mysql_query(c, "ALTER TABLE users DROP INDEX IF EXISTS username");
    mysql_query(c,
                "ALTER TABLE users ADD UNIQUE INDEX idx_users_user_ver "
                "(username, token_version)");

    // Outbox composite index: accelerates SELECT status=0 ORDER BY id LIMIT N
    mysql_query(c, "CREATE INDEX idx_outbox_status ON outbox(status, id)");

    // INT -> BIGINT upgrade, backward compatible, MySQL 8.0 supports ALGORITHM=INSTANT
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

    }  // end DDL loop over write_conns_

    // Read connections also need DDL 锟?they may connect before tables are created.
    // DDL is global but running it on each connection guarantees visibility.
    for (auto &rcp : read_conns_) {
        if (!rcp->conn) continue;
        std::lock_guard<std::mutex> lock(rcp->mtx);
        MYSQL *c = rcp->conn.get();
        auto exec = [&](const char *sql) {
            if (mysql_query(c, sql) != 0)
                fprintf(stderr, "[DB] DDL error on read conn: %s\n", mysql_error(c));
        };
        exec("CREATE TABLE IF NOT EXISTS users (id BIGINT AUTO_INCREMENT PRIMARY KEY, username VARCHAR(64) UNIQUE NOT NULL, password_hash VARCHAR(256) NOT NULL, created_at DATETIME NOT NULL DEFAULT NOW())");
        exec("CREATE TABLE IF NOT EXISTS spreadsheets (id BIGINT AUTO_INCREMENT PRIMARY KEY, username VARCHAR(64) NOT NULL, name VARCHAR(255) NOT NULL, description TEXT, headers_json JSON NOT NULL, data_json JSON NOT NULL, row_count INT NOT NULL DEFAULT 0, col_count INT NOT NULL DEFAULT 0, created_at DATETIME NOT NULL DEFAULT NOW(), updated_at DATETIME NOT NULL DEFAULT NOW())");
        exec("CREATE TABLE IF NOT EXISTS files (id BIGINT AUTO_INCREMENT PRIMARY KEY, username VARCHAR(64) NOT NULL, original_name VARCHAR(512) NOT NULL, size BIGINT NOT NULL DEFAULT 0, mime_type VARCHAR(128) DEFAULT '', file_content LONGBLOB, created_at DATETIME NOT NULL DEFAULT NOW())");
        exec("CREATE TABLE IF NOT EXISTS outbox (id BIGINT AUTO_INCREMENT PRIMARY KEY, event_type VARCHAR(64) NOT NULL, payload JSON NOT NULL, created_at DATETIME DEFAULT NOW())");
    }

    printf("[DB] schema initialized on %s (conns: write=%zu read=%zu)\n",
           db_name_.c_str(), write_conns_.size(), read_conns_.size());
    return true;
}

// ---- Users ----

bool Database::AddUser(const std::string &username, const std::string &password_hash) {
    MYSQL *ec = EscConn();
    int64_t sf_id = snowflake_ ? snowflake_->Next() : 0;
    return ExecWrite(make_sql("INSERT INTO users (id, username, password_hash) VALUES ({},{},{})",
                              sf_id, sql_param(ec, username), sql_param(ec, password_hash)));
}

bool Database::GetUser(const std::string &username, std::string &password_hash_out) {
    // 寮轰竴锟? 瀵嗙爜楠岃瘉蹇呴』璇讳富锟?
    auto h = GetHealthyWriteConn();
    if (!h)
        return false;
    std::string sql = make_sql("SELECT password_hash FROM users WHERE username={}",
                                sql_param(h.conn, username));
    MYSQL_RES *res = nullptr;
    if (!RunQuery(h.conn, sql, &res))
        return false;
    MYSQL_ROW row = mysql_fetch_row(res);
    bool ok = row != nullptr;
    if (ok)
        password_hash_out = row[0] ? row[0] : "";
    mysql_free_result(res);
    return ok;
}

bool Database::UserExists(const std::string &username) {
    // 寮轰竴锟? 鏌ラ噸蹇呴』璧颁富锟? 閬垮厤涓讳粠寤惰繜瀵艰嚧閲嶅娉ㄥ唽
    auto h = GetHealthyWriteConn();
    if (!h)
        return false;
    std::string sql = make_sql("SELECT 1 FROM users WHERE username={}",
                                sql_param(h.conn, username));
    MYSQL_RES *res = nullptr;
    if (!RunQuery(h.conn, sql, &res))
        return false;
    bool exists = mysql_fetch_row(res) != nullptr;
    mysql_free_result(res);
    return exists;
}

int Database::GetTokenVersion(const std::string &username) {
    // 寮轰竴锟? token 鐗堟湰鍙峰繀椤昏涓诲簱
    auto h = GetHealthyWriteConn();
    if (!h)
        return -1;
    std::string sql = make_sql("SELECT token_version FROM users WHERE username={}",
                                sql_param(h.conn, username));
    MYSQL_RES *res = nullptr;
    if (!RunQuery(h.conn, sql, &res))
        return -1;
    MYSQL_ROW row = mysql_fetch_row(res);
    int ver = (row && row[0]) ? std::stoi(row[0]) : -1;
    mysql_free_result(res);
    return ver;
}

bool Database::IncrementTokenVersion(const std::string &username) {
    MYSQL *ec = EscConn();
    return ExecWrite(make_sql("UPDATE users SET token_version = token_version + 1 WHERE username={}",
                              sql_param(ec, username)));
}

int64_t Database::GetUserId(const std::string &username) {
    // Must read master to avoid replication lag causing stale id on fresh
    // registration.
    auto h = GetHealthyWriteConn();
    if (!h)
        return -1;
    std::string sql = make_sql("SELECT id FROM users WHERE username={}",
                                sql_param(h.conn, username));
    MYSQL_RES *res = nullptr;
    if (!RunQuery(h.conn, sql, &res))
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

// ---- Spreadsheets ----

bool Database::CreateSpreadsheet(int64_t user_id, const std::string &username, const std::string &name,
                                 const std::string &desc, const std::string &headers_json, const std::string &data_json,
                                 int64_t &out_id, const std::string &idempotency_key) {
    MYSQL *ec = EscConn();
    int64_t sf_id = snowflake_ ? snowflake_->Next() : 0;
    int rc = countRows(data_json), cc = countCols(headers_json);
    if (!idempotency_key.empty()) {
        return ExecWriteInsert(make_sql(
            "INSERT INTO spreadsheets (id,user_id,username,name,description,"
            "headers_json,data_json,row_count,col_count,idempotency_key) VALUES "
            "({},{},{},{},{},{},{},{},{},{}) ON DUPLICATE KEY UPDATE id=LAST_INSERT_ID(id)",
            sf_id, user_id, sql_param(ec, username), sql_param(ec, name),
            sql_param(ec, desc), sql_param(ec, headers_json), sql_param(ec, data_json),
            rc, cc, sql_param(ec, idempotency_key)), out_id);
    }
    return ExecWriteInsert(make_sql(
        "INSERT INTO spreadsheets (id,user_id,username,name,description,"
        "headers_json,data_json,row_count,col_count) VALUES "
        "({},{},{},{},{},{},{},{},{})",
        sf_id, user_id, sql_param(ec, username), sql_param(ec, name),
        sql_param(ec, desc), sql_param(ec, headers_json), sql_param(ec, data_json),
        rc, cc), out_id);
}

bool Database::GetSpreadsheet(int64_t id, int64_t user_id, SpreadsheetRow &out) {
    if (user_id <= 0)
        return false;
    std::string sql = make_sql(
        "SELECT id,username,name,description,headers_json,data_json,"
        "row_count,col_count,created_at,updated_at,version,storage_path FROM "
        "spreadsheets WHERE id={} AND user_id={}", id, user_id);
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

bool Database::ListSpreadsheets(int64_t user_id, std::vector<SpreadsheetSummary> &out,
                                std::string &next_cursor, bool &has_more,
                                int limit, const std::string &cursor) {
    if (limit <= 0) limit = 20;
    int64_t cursor_id = cursor.empty() ? INT64_MAX : std::stoll(cursor);
    std::string where = make_sql("WHERE user_id={} AND id<{}", user_id, cursor_id);

    // Fetch limit+1 rows to detect has_more without COUNT(*)
    std::string sql =
        "SELECT s.id,s.name,s.description,s.row_count,s.col_count,s.updated_at "
        "FROM spreadsheets s "
        "JOIN (SELECT id FROM spreadsheets " +
        where + make_sql(" ORDER BY id DESC LIMIT {}", limit + 1) +
        ") tmp ON s.id=tmp.id "
        "ORDER BY s.id DESC";

    has_more = false;
    next_cursor.clear();
    return ExecRead(sql, [&](MYSQL_RES *res) {
        MYSQL_ROW row;
        int count = 0;
        while ((row = mysql_fetch_row(res))) {
            if (count >= limit) {
                has_more = true;
                break;
            }
            SpreadsheetSummary s;
            s.id = std::stoll(row[0]);
            s.name = row[1] ? row[1] : "";
            s.description = row[2] ? row[2] : "";
            s.row_count = row[3] ? std::stoi(row[3]) : 0;
            s.col_count = row[4] ? std::stoi(row[4]) : 0;
            s.updated_at = row[5] ? row[5] : "";
            out.push_back(s);
            count++;
        }
        if (!out.empty())
            next_cursor = std::to_string(out.back().id);
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
    std::string sql = make_sql(
        "UPDATE spreadsheets SET name={},description={},headers_json={},data_json={},"
        "row_count={},col_count={},version = version + 1, updated_at=NOW() WHERE id={}",
        sql_param(ec, name), sql_param(ec, desc), sql_param(ec, headers_json),
        sql_param(ec, data_json), rc, cc, id);
    if (version > 0) {
        sql += " AND version = " + std::to_string(version);
    }
    // 涔愯閿佸啿绐佹椂 affected_rows=0 锟?ExecWrite 杩斿洖 true(锟?SQL 閿欒)
    // 涓氬姟灞傞€氳繃杩斿洖鍊兼棤娉曞尯锟?鏈尮锟?锟?鎴愬姛", 闇€棰濆妫€锟?
    // 杩欓噷鏀逛负: version>0 鏃舵锟?affected_rows, 涓嶅尮閰嶅垯杩斿洖 false
    if (version > 0) {
        // 璧板甫鐗堟湰鍙风殑鏇存柊, 闇€瑕佹鏌ュ彈褰卞搷琛屾暟
        auto h = GetHealthyWriteConn();
        if (!h)
            return false;
        if (mysql_query(h.conn, sql.c_str()) != 0) {
            fprintf(stderr, "[DB:write] UpdateSpreadsheet error: %s\n", mysql_error(h.conn));
            return false;
        }
        return mysql_affected_rows(h.conn) > 0;
    }
    return ExecWrite(sql);
}

bool Database::DeleteSpreadsheet(int64_t id, int64_t /*user_id*/) {
    return ExecWrite(make_sql("DELETE FROM spreadsheets WHERE id={}", id));
}

bool Database::GetSpreadsheetOwner(int64_t id, int64_t &owner_user_id, int *out_version) {
    // 寮轰竴锟? 褰掑睘鏍￠獙蹇呴』璇讳富锟?
    auto h = GetHealthyWriteConn();
    if (!h)
        return false;
    std::string sql = make_sql("SELECT user_id, version FROM spreadsheets WHERE id={}", id);
    MYSQL_RES *res = nullptr;
    if (!RunQuery(h.conn, sql, &res))
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
    int64_t sf_id = snowflake_ ? snowflake_->Next() : 0;
    if (!idempotency_key.empty()) {
        if (!storage_key.empty()) {
            return ExecWriteInsert(make_sql(
                "INSERT INTO files (id,user_id,username,original_name,size,mime_type,"
                "storage_path,idempotency_key) VALUES ({},{},{},{},{},{},{},{})"
                " ON DUPLICATE KEY UPDATE id=LAST_INSERT_ID(id)",
                sf_id, user_id, sql_param(ec, username), sql_param(ec, original_name),
                size, sql_param(ec, mime_type), sql_param(ec, storage_key),
                sql_param(ec, idempotency_key)), out_id);
        }
        return ExecWriteInsert(make_sql(
            "INSERT INTO files (id,user_id,username,original_name,size,mime_type,"
            "idempotency_key) VALUES ({},{},{},{},{},{},{})"
            " ON DUPLICATE KEY UPDATE id=LAST_INSERT_ID(id)",
            sf_id, user_id, sql_param(ec, username), sql_param(ec, original_name),
            size, sql_param(ec, mime_type), sql_param(ec, idempotency_key)), out_id);
    }
    if (!storage_key.empty()) {
        return ExecWriteInsert(make_sql(
            "INSERT INTO files (id,user_id,username,original_name,size,mime_type,"
            "storage_path) VALUES ({},{},{},{},{},{},{})",
            sf_id, user_id, sql_param(ec, username), sql_param(ec, original_name),
            size, sql_param(ec, mime_type), sql_param(ec, storage_key)), out_id);
    }
    return ExecWriteInsert(make_sql(
        "INSERT INTO files (id,user_id,username,original_name,size,mime_type)"
        " VALUES ({},{},{},{},{},{})",
        sf_id, user_id, sql_param(ec, username), sql_param(ec, original_name),
        size, sql_param(ec, mime_type)), out_id);
}

bool Database::UpdateFileContent(int64_t id, const std::string &content, int version) {
    auto h = GetHealthyWriteConn();
    if (!h)
        return false;
    std::string sql = make_sql(
        "UPDATE files SET file_content={}, version = version + 1 WHERE id={}",
        sql_param(h.conn, content), id);
    if (version > 0)
        sql += make_sql(" AND version={}", version);
    if (mysql_query(h.conn, sql.c_str()) != 0) {
        fprintf(stderr, "[DB] UpdateFileContent error: %s\n", mysql_error(h.conn));
        return false;
    }
    if (version > 0)
        return mysql_affected_rows(h.conn) > 0;
    return true;
}

bool Database::GetFile(int64_t id, int64_t user_id, FileRow &out) {
    if (user_id <= 0)
        return false;
    // storage_path is col 7: if non-empty, caller should fetch content from
    // object storage instead of reading the legacy file_content LONGBLOB (col 6)
    std::string sql = make_sql(
        "SELECT id,username,original_name,size,mime_type,created_at,"
        "file_content,storage_path,version FROM files WHERE id={} AND user_id={}",
        id, user_id);
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
        out.version = row[8] ? std::stoi(row[8]) : 1;
        if (out.storage_path.empty() && row[6]) {
            unsigned long *lens = mysql_fetch_lengths(res);
            if (lens)
                out.file_content.assign(row[6], lens[6]);
        }
        return true;
    });
}

bool Database::ListFiles(int64_t user_id, std::vector<FileRow> &out,
                         std::string &next_cursor, bool &has_more,
                         int limit, const std::string &cursor) {
    if (limit <= 0) limit = 20;
    int64_t cursor_id = cursor.empty() ? INT64_MAX : std::stoll(cursor);
    std::string where = make_sql("WHERE user_id={} AND id<{}", user_id, cursor_id);

    // Fetch limit+1 rows to detect has_more without COUNT(*)
    std::string sql =
        "SELECT "
        "f.id,f.username,f.original_name,f.size,f.mime_type,f.created_at "
        "FROM files f "
        "JOIN (SELECT id FROM files " +
        where + make_sql(" ORDER BY id DESC LIMIT {}", limit + 1) +
        ") tmp ON f.id=tmp.id "
        "ORDER BY f.id DESC";

    has_more = false;
    next_cursor.clear();
    return ExecRead(sql, [&](MYSQL_RES *res) {
        MYSQL_ROW row;
        int count = 0;
        while ((row = mysql_fetch_row(res))) {
            if (count >= limit) {
                has_more = true;
                break;
            }
            FileRow f;
            f.id = std::stoll(row[0]);
            f.username = row[1] ? row[1] : "";
            f.original_name = row[2] ? row[2] : "";
            f.size = row[3] ? std::stoll(row[3]) : 0;
            f.mime_type = row[4] ? row[4] : "";
            f.created_at = row[5] ? row[5] : "";
            out.push_back(f);
            count++;
        }
        if (!out.empty())
            next_cursor = std::to_string(out.back().id);
        return true;
    });
}

bool Database::DeleteFile(int64_t id, int64_t /*user_id*/) {
    return ExecWrite(make_sql("DELETE FROM files WHERE id={}", id));
}

bool Database::GetFileOwner(int64_t id, int64_t &owner_user_id) {
    // 寮轰竴锟? 褰掑睘鏍￠獙蹇呴』璇讳富锟?
    auto h = GetHealthyWriteConn();
    if (!h)
        return false;
    std::string sql = make_sql("SELECT user_id FROM files WHERE id={}", id);
    MYSQL_RES *res = nullptr;
    if (!RunQuery(h.conn, sql, &res))
        return false;
    MYSQL_ROW row = mysql_fetch_row(res);
    if (row)
        owner_user_id = row[0] ? std::stoll(row[0]) : 0;
    mysql_free_result(res);
    return row != nullptr;
}

bool Database::GetFileStoragePath(int64_t id, std::string &storage_path) {
    // 鏌ヨ MinIO object key锛岀敤浜庡垹闄ゆ椂涓€骞舵竻鐞嗗璞″瓨锟?
    storage_path.clear();
    std::string sql = make_sql("SELECT storage_path FROM files WHERE id={}", id);
    return ExecRead(sql, [&](MYSQL_RES *res) {
        MYSQL_ROW row = mysql_fetch_row(res);
        if (row && row[0])
            storage_path = row[0];
        return row != nullptr;
    });
}

bool Database::GetSpreadsheetStoragePath(int64_t id, std::string &storage_path) {
    storage_path.clear();
    std::string sql = make_sql("SELECT storage_path FROM spreadsheets WHERE id={}", id);
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
    return ExecWrite(make_sql("UPDATE spreadsheets SET storage_path={} WHERE id={}",
                              sql_param(ec, storage_path), id));
}

// ---- Undo Log (always master 锟?2PC requires consistency) ----

bool Database::WriteUndoLog(const std::string &xid, const std::string &table_name, int64_t row_id,
                            const std::string &before_snapshot) {
    MYSQL *ec = EscConn();
    return ExecWrite(make_sql(
        "INSERT INTO undo_log (xid,table_name,row_id,before_snapshot) VALUES ({},{},{},{})",
        sql_param(ec, xid), sql_param(ec, table_name), row_id, sql_param(ec, before_snapshot)));
}

bool Database::GetUndoLog(const std::string &xid, std::string &table_name, int64_t &row_id,
                          std::string &before_snapshot) {
    // 2PC 涓€鑷达拷? undo_log 蹇呴』璇讳富锟?
    auto h = GetHealthyWriteConn();
    if (!h)
        return false;
    std::string sql = make_sql(
        "SELECT table_name,row_id,before_snapshot FROM undo_log "
        "WHERE xid={} ORDER BY id DESC LIMIT 1",
        sql_param(h.conn, xid));
    MYSQL_RES *res = nullptr;
    if (!RunQuery(h.conn, sql, &res))
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
    return ExecWrite(make_sql("DELETE FROM undo_log WHERE xid={}", sql_param(ec, xid)));
}

int Database::PurgeOldUndoLogs(int days) {
    std::string sql = make_sql("DELETE FROM undo_log WHERE created_at < NOW() - INTERVAL {} DAY", days);
    auto h = GetHealthyWriteConn();
    if (!h)
        return -1;
    if (mysql_query(h.conn, sql.c_str()) != 0) {
        fprintf(stderr, "[DB] PurgeOldUndoLogs error: %s\n", mysql_error(h.conn));
        return -1;
    }
    int affected = (int)mysql_affected_rows(h.conn);
    printf("[DB] PurgeOldUndoLogs: removed %d rows older than %d days\n", affected, days);
    return affected;
}

// ---- 杩炴帴姹犲仴搴锋锟?----

void Database::StartHealthCheck() {
    if (health_running_.exchange(true))
        return;  // 宸插湪杩愯锛岄槻姝㈤噸澶嶈皟鐢ㄥ锟?std::terminate
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

        // 鈥斺€旓拷?锟?1 锟? 鍏ㄩ儴 ping, 姝昏繛鎺ラ噸锟?鈥斺€旓拷?
        for (size_t i = 0; i < write_conns_.size(); ++i) {
            auto &wc = write_conns_[i];
            std::lock_guard<std::mutex> lock(wc->mtx);
            if (wc->conn && mysql_ping(wc->conn.get()) != 0) {
                fprintf(stderr, "[DB:health] write conn %zu dead, reconnecting\n", i);
                wc->conn = ConnectMYSQL(write_host_, write_port_);
                if (wc->conn)
                    fprintf(stderr, "[DB:health] write conn %zu reconnected\n", i);
            }
        }

        for (size_t i = 0; i < read_conns_.size(); ++i) {
            auto &rc = read_conns_[i];
            std::lock_guard<std::mutex> lock(rc->mtx);
            if (rc->conn && mysql_ping(rc->conn.get()) != 0) {
                fprintf(stderr, "[DB:health] read conn %zu dead, reconnecting\n", i);
                rc->conn = ConnectMYSQL(read_hosts_[i % read_hosts_.size()], read_port_);
                if (rc->conn)
                    fprintf(stderr, "[DB:health] read conn %zu reconnected\n", i);
            }
        }

        // 鈥斺€旓拷?锟?2 锟? 绌洪棽杩炴帴娣樻卑 鈥斺€旓拷?
        int min_idle = min_idle_.load();
        int idle_timeout_ms = idle_timeout_ms_.load();
        if (min_idle <= 0 && idle_timeout_ms <= 0)
            continue;  // 鏈惎鐢ㄧ┖闂叉窐锟?

        int64_t now_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
                .count();

        auto evictPool = [&](auto &pool, const char *label) {
            // 缁熻瀛樻椿杩炴帴锟?
            int alive = 0;
            for (auto &pc : pool)
                if (pc->conn)
                    alive++;

            if (alive <= min_idle)
                return;  // 宸插埌淇濆簳锟? 涓嶆窐锟?

            for (size_t i = 0; i < pool.size(); ++i) {
                if (alive <= min_idle)
                    break;
                auto &pc = pool[i];
                int64_t last = pc->last_used_ms.load();
                if (pc->conn && (now_ms - last) > idle_timeout_ms) {
                    std::lock_guard<std::mutex> lock(pc->mtx);
                    if (pc->conn && (now_ms - pc->last_used_ms.load()) > idle_timeout_ms) {
                        fprintf(stderr, "[DB:health] %s conn %zu idle %lds, closing\n",
                                label, i, (now_ms - last) / 1000);
                        pc->conn.reset();
                        alive--;
                    }
                }
            }
        };

        evictPool(write_conns_, "write");
        evictPool(read_conns_, "read");
    }
}

// ============================================================
// ShardedDatabase 锟?hash-routes user_id across N Database shards
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

// === Users (auth shard only 锟?shard 0) ===

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
bool ShardedDatabase::ListSpreadsheets(int64_t user_id, std::vector<SpreadsheetSummary> &out,
                                       std::string &next_cursor, bool &has_more,
                                       int limit, const std::string &cursor) {
    return ShardFor(user_id)->ListSpreadsheets(user_id, out, next_cursor, has_more, limit, cursor);
}
bool ShardedDatabase::UpdateSpreadsheet(int64_t id, int64_t user_id, const std::string &name, const std::string &desc,
                                        const std::string &headers_json, const std::string &data_json, int version) {
    return ShardFor(user_id)->UpdateSpreadsheet(id, name, desc, headers_json, data_json, version);
}
bool ShardedDatabase::UpdateSpreadsheet(int64_t id, const std::string &name, const std::string &desc,
                                        const std::string &headers_json, const std::string &data_json, int version) {
    // Legacy: no user_id 锟?broadcast
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
bool ShardedDatabase::UpdateFileContent(int64_t id, const std::string &content, int version) {
    return ShardFor(id)->UpdateFileContent(id, content, version);
}
bool ShardedDatabase::GetFile(int64_t id, int64_t user_id, FileRow &out) {
    return ShardFor(user_id)->GetFile(id, user_id, out);
}
bool ShardedDatabase::ListFiles(int64_t user_id, std::vector<FileRow> &out,
                                std::string &next_cursor, bool &has_more,
                                int limit, const std::string &cursor) {
    return ShardFor(user_id)->ListFiles(user_id, out, next_cursor, has_more, limit, cursor);
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
    if (!trace_context.empty()) {
        return ExecWrite(make_sql(
            "INSERT INTO outbox (event_type,payload,trace_context) VALUES ({},{},{})",
            sql_param(ec, event_type), sql_param(ec, payload), sql_param(ec, trace_context)));
    }
    return ExecWrite(make_sql("INSERT INTO outbox (event_type,payload) VALUES ({},{})",
                              sql_param(ec, event_type), sql_param(ec, payload)));
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

// === Undo Log (broadcast 锟?2PC 涓€鑷存€ц锟? ===

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
    std::string sql = make_sql("SELECT id FROM users WHERE phone={}", sql_param(EscConn(), phone));
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
    std::string sql = make_sql("SELECT username FROM users WHERE id={}", user_id);
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
    std::string sql = make_sql("SELECT password_hash FROM users WHERE id={}", user_id);
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
    return ExecWrite(make_sql("UPDATE users SET password_hash={} WHERE id={}",
                              sql_param(ec, new_hash), user_id));
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
    return ExecWriteInsert(make_sql(
        "INSERT INTO files (user_id,username,original_name,size,mime_type,"
        "folder_id,is_folder,file_content) VALUES ({},'folder',{},0,'',{},1,'')",
        user_id, sql_param(ec, name), parent_id), out_id);
}
bool Database::MoveFile(int64_t id, int64_t target_folder_id, int version) {
    std::string sql = make_sql(
        "UPDATE files SET folder_id={}, version = version + 1 WHERE id={}",
        target_folder_id, id);
    if (version > 0)
        sql += make_sql(" AND version={}", version);
    if (version > 0) {
        auto h = GetHealthyWriteConn();
        if (!h)
            return false;
        if (mysql_query(h.conn, sql.c_str()) != 0)
            return false;
        return mysql_affected_rows(h.conn) > 0;
    }
    return ExecWrite(sql);
}
int Database::BatchDeleteFiles(int64_t user_id, const std::vector<int64_t> &ids) {
    if (ids.empty())
        return 0;
    // 鍗曟潯 SQL锛屽師瀛愭墽琛岋紝閬垮厤閮ㄥ垎鎴愬姛閮ㄥ垎澶辫触
    std::string in_list;
    for (size_t i = 0; i < ids.size(); ++i) {
        if (i > 0)
            in_list += ",";
        in_list += std::to_string(ids[i]);
    }
    std::string sql = "DELETE FROM files WHERE id IN (" + in_list +
                      ") AND user_id=" + std::to_string(user_id);
    if (!ExecWrite(sql))
        return 0;
    return (int)ids.size();
}
bool ShardedDatabase::CreateFolder(int64_t user_id, const std::string &name, int64_t parent_id, int64_t &out_id) {
    return ShardFor(user_id)->CreateFolder(user_id, name, parent_id, out_id);
}
bool ShardedDatabase::MoveFile(int64_t id, int64_t target_folder_id, int version) {
    for (auto &db : shards_)
        if (db->MoveFile(id, target_folder_id, version))
            return true;
    return false;
}
int ShardedDatabase::BatchDeleteFiles(int64_t user_id, const std::vector<int64_t> &ids) {
    return ShardFor(user_id)->BatchDeleteFiles(user_id, ids);
}

// ---- Resource Sharing ----

bool Database::EnsureSharingTables() {
    const char *sql_shares =
        "CREATE TABLE IF NOT EXISTS resource_shares ("
        "  id BIGINT AUTO_INCREMENT PRIMARY KEY,"
        "  owner_id BIGINT NOT NULL,"
        "  resource_type VARCHAR(32) NOT NULL,"
        "  resource_id BIGINT NOT NULL,"
        "  grantee_username VARCHAR(64) NOT NULL,"
        "  permission VARCHAR(16) NOT NULL DEFAULT 'view',"
        "  granted_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,"
        "  UNIQUE KEY uk_share (owner_id, resource_type, resource_id, grantee_username)"
        ")";
    const char *sql_links =
        "CREATE TABLE IF NOT EXISTS share_links ("
        "  token VARCHAR(64) PRIMARY KEY,"
        "  owner_id BIGINT NOT NULL,"
        "  resource_type VARCHAR(32) NOT NULL,"
        "  resource_id BIGINT NOT NULL,"
        "  permission VARCHAR(16) NOT NULL DEFAULT 'view',"
        "  created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP"
        ")";
    return ExecWrite(sql_shares) && ExecWrite(sql_links);
}

static std::string make_token() {
    static const char hex[] = "0123456789abcdef";
    std::string tok(32, '\0');
    for (size_t i = 0; i < 32; ++i)
        tok[i] = hex[(rand() >> 4) & 15];
    return tok;
}

bool Database::CreateResourceShare(int64_t owner_id, const std::string &resource_type, int64_t resource_id,
                                    const std::string &grantee_username, const std::string &permission) {
    MYSQL *ec = EscConn();
    return ExecWrite(make_sql(
        "INSERT INTO resource_shares (owner_id,resource_type,resource_id,grantee_username,permission) "
        "VALUES ({},{},{},{},{}) ON DUPLICATE KEY UPDATE permission={}",
        owner_id, sql_param(ec, resource_type), resource_id,
        sql_param(ec, grantee_username), sql_param(ec, permission),
        sql_param(ec, permission)));
}

bool Database::RevokeResourceShare(int64_t owner_id, const std::string &resource_type, int64_t resource_id,
                                    const std::string &grantee_username) {
    MYSQL *ec = EscConn();
    return ExecWrite(make_sql(
        "DELETE FROM resource_shares WHERE owner_id={} AND resource_type={} AND resource_id={} AND grantee_username={}",
        owner_id, sql_param(ec, resource_type), resource_id, sql_param(ec, grantee_username)));
}

bool Database::ListResourceShares(int64_t owner_id, const std::string &resource_type, int64_t resource_id,
                                   std::string &out_entries_json) {
    MYSQL *ec = EscConn();
    std::string sql = make_sql(
        "SELECT grantee_username, permission, granted_at FROM resource_shares "
        "WHERE owner_id={} AND resource_type={} AND resource_id={} ORDER BY granted_at",
        owner_id, sql_param(ec, resource_type), resource_id);
    std::string entries = "[";
    bool first = true;
    ExecRead(sql, [&](MYSQL_RES *res) -> bool {
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(res))) {
            if (!first) entries += ",";
            first = false;
            entries += "{\"username\":\"" + std::string(row[0] ? row[0] : "") + "\""
                     + ",\"permission\":\"" + std::string(row[1] ? row[1] : "") + "\""
                     + ",\"granted_at\":\"" + std::string(row[2] ? row[2] : "") + "\"}";
        }
        return true;
    });
    entries += "]";
    out_entries_json = entries;
    return true;
}

bool Database::CheckShareAccess(const std::string &username, const std::string &resource_type, int64_t resource_id,
                                 std::string &out_permission) {
    MYSQL *ec = EscConn();
    std::string sql = make_sql(
        "SELECT permission FROM resource_shares WHERE grantee_username={} AND resource_type={} AND resource_id={}",
        sql_param(ec, username), sql_param(ec, resource_type), resource_id);
    bool found = false;
    ExecRead(sql, [&](MYSQL_RES *res) -> bool {
        MYSQL_ROW row = mysql_fetch_row(res);
        if (row && row[0]) {
            out_permission = row[0];
            found = true;
        }
        return true;
    });
    return found;
}

bool Database::CreateShareLink(int64_t owner_id, const std::string &resource_type, int64_t resource_id,
                                const std::string &permission, std::string &out_token) {
    MYSQL *ec = EscConn();
    out_token = make_token();
    return ExecWrite(make_sql(
        "INSERT INTO share_links (token,owner_id,resource_type,resource_id,permission) VALUES ({},{},{},{},{})",
        sql_param(ec, out_token), owner_id, sql_param(ec, resource_type), resource_id,
        sql_param(ec, permission)));
}

bool Database::GetShareLinkByToken(const std::string &token, std::string &resource_type, int64_t &resource_id,
                                    std::string &permission, int64_t &owner_id) {
    MYSQL *ec = EscConn();
    std::string sql = make_sql(
        "SELECT resource_type, resource_id, permission, owner_id FROM share_links WHERE token={}",
        sql_param(ec, token));
    bool found = false;
    ExecRead(sql, [&](MYSQL_RES *res) -> bool {
        MYSQL_ROW row = mysql_fetch_row(res);
        if (row && row[0]) {
            resource_type = row[0] ? row[0] : "";
            resource_id = row[1] ? std::stoll(row[1]) : 0;
            permission = row[2] ? row[2] : "";
            owner_id = row[3] ? std::stoll(row[3]) : 0;
            found = true;
        }
        return true;
    });
    return found;
}

// ===== Workspace =====

bool Database::EnsureWorkspaceTables() {
    return ExecWrite(
        "CREATE TABLE IF NOT EXISTS workspaces ("
        "  id BIGINT PRIMARY KEY,"
        "  name VARCHAR(128) NOT NULL,"
        "  owner_id BIGINT NOT NULL,"
        "  created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP"
        ")") &&
    ExecWrite(
        "CREATE TABLE IF NOT EXISTS workspace_members ("
        "  workspace_id BIGINT NOT NULL,"
        "  user_id BIGINT NOT NULL,"
        "  username VARCHAR(64) NOT NULL,"
        "  role VARCHAR(16) NOT NULL DEFAULT 'viewer',"
        "  joined_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,"
        "  PRIMARY KEY (workspace_id, user_id)"
        ")");
}

bool Database::CreateWorkspace(int64_t owner_id, const std::string &name, int64_t &out_id) {
    MYSQL *ec = EscConn();
    return ExecWriteInsert(make_sql(
        "INSERT INTO workspaces (id,name,owner_id) VALUES ({},{},{})",
        owner_id, sql_param(ec, name), owner_id), out_id);
}

bool Database::GetWorkspace(int64_t id, std::string &name, int64_t &owner_id, std::string &created_at) {
    bool found = false;
    ExecRead(make_sql("SELECT name,owner_id,created_at FROM workspaces WHERE id={}", id),
             [&](MYSQL_RES *res) -> bool {
                 MYSQL_ROW row = mysql_fetch_row(res);
                 if (row) {
                     name = row[0] ? row[0] : "";
                     owner_id = row[1] ? std::stoll(row[1]) : 0;
                     created_at = row[2] ? row[2] : "";
                     found = true;
                 }
                 return true;
             });
    return found;
}

bool Database::ListWorkspaces(int64_t user_id, std::string &out_json) {
    std::vector<nlohmann::json> items;
    ExecRead(make_sql(
        "SELECT w.id,w.name,w.owner_id,w.created_at FROM workspaces w "
        "INNER JOIN workspace_members m ON w.id=m.workspace_id "
        "WHERE m.user_id={} ORDER BY w.created_at DESC", user_id),
        [&](MYSQL_RES *res) -> bool {
            MYSQL_ROW row;
            while ((row = mysql_fetch_row(res))) {
                nlohmann::json j;
                j["id"] = row[0] ? std::stoll(row[0]) : 0;
                j["name"] = row[1] ? row[1] : "";
                j["owner_id"] = row[2] ? std::stoll(row[2]) : 0;
                j["created_at"] = row[3] ? row[3] : "";
                items.push_back(j);
            }
            return true;
        });
    out_json = nlohmann::json(items).dump();
    return true;
}

bool Database::UpdateWorkspace(int64_t id, const std::string &name) {
    MYSQL *ec = EscConn();
    return ExecWrite(make_sql("UPDATE workspaces SET name={} WHERE id={}", sql_param(ec, name), id));
}

bool Database::DeleteWorkspace(int64_t id) {
    ExecWrite(make_sql("DELETE FROM workspace_members WHERE workspace_id={}", id));
    return ExecWrite(make_sql("DELETE FROM workspaces WHERE id={}", id));
}

bool Database::AddWorkspaceMember(int64_t wid, int64_t uid, const std::string &username, const std::string &role) {
    MYSQL *ec = EscConn();
    return ExecWrite(make_sql(
        "INSERT INTO workspace_members (workspace_id,user_id,username,role) VALUES ({},{},{},{}) "
        "ON DUPLICATE KEY UPDATE role={}",
        wid, uid, sql_param(ec, username), sql_param(ec, role), sql_param(ec, role)));
}

bool Database::RemoveWorkspaceMember(int64_t wid, int64_t uid) {
    return ExecWrite(make_sql("DELETE FROM workspace_members WHERE workspace_id={} AND user_id={}", wid, uid));
}

bool Database::IsWorkspaceOwner(int64_t wid, int64_t uid) {
    bool found = false;
    ExecRead(make_sql("SELECT 1 FROM workspaces WHERE id={} AND owner_id={}", wid, uid),
             [&](MYSQL_RES *res) -> bool { found = (mysql_fetch_row(res) != nullptr); return true; });
    return found;
}

bool Database::GetWorkspaceMemberRole(int64_t wid, int64_t uid, std::string &out_role) {
    bool found = false;
    ExecRead(make_sql("SELECT role FROM workspace_members WHERE workspace_id={} AND user_id={}", wid, uid),
             [&](MYSQL_RES *res) -> bool {
                 MYSQL_ROW row = mysql_fetch_row(res);
                 if (row) { out_role = row[0] ? row[0] : ""; found = true; }
                 return true;
             });
    return found;
}

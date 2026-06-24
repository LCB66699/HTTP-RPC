// 迁移脚本：将存量 spreadsheets 的 cells/headers 从 MySQL data_json 列导入 MongoDB
// 编译: g++ -std=c++17 -o migrate_sheets_to_mongo tools/migrate_sheets_to_mongo.cpp \
//         server/src/shared/mongo_client.cpp \
//         $(pkg-config --cflags --libs libmongocxx mysqlclient nlohmann_json)
// 运行: ./migrate_sheets_to_mongo [mysql_host] [mongo_uri]

#include <mysql/mysql.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../server/include/shared/client/mongo_client.h"

int main(int argc, char *argv[]) {
    const char *mysql_host = argc > 1 ? argv[1] : "mysql-spreadsheet-0";
    const char *mysql_user = "root";
    const char *mysql_pass = getenv("MYSQL_ROOT_PASSWORD") ? getenv("MYSQL_ROOT_PASSWORD") : "123456";
    const char *mysql_db = "rpc_spreadsheet";
    const char *mongo_uri = argc > 2 ? argv[2] : "mongodb://mongodb:27017";

    printf("[Migrate] Connecting to MySQL %s...\n", mysql_host);
    MYSQL *conn = mysql_init(nullptr);
    if (!mysql_real_connect(conn, mysql_host, mysql_user, mysql_pass, mysql_db, 3306, nullptr, 0)) {
        fprintf(stderr, "[Migrate] MySQL connect failed: %s\n", mysql_error(conn));
        return 1;
    }

    MongoClient mongo(mongo_uri, "rpc_sheets");
    if (!mongo.Connect()) {
        fprintf(stderr, "[Migrate] MongoDB connect failed\n");
        mysql_close(conn);
        return 1;
    }

    if (mysql_query(conn, "SELECT id, user_id, headers_json, data_json FROM spreadsheets")) {
        fprintf(stderr, "[Migrate] Query failed: %s\n", mysql_error(conn));
        mysql_close(conn);
        return 1;
    }

    MYSQL_RES *result = mysql_store_result(conn);
    int total = mysql_num_rows(result);
    int migrated = 0, skipped = 0;

    printf("[Migrate] Found %d spreadsheets\n", total);

    MYSQL_ROW row;
    while ((row = mysql_fetch_row(result))) {
        int64_t id = atoll(row[0]);
        int64_t user_id = row[1] ? atoll(row[1]) : 0;
        const char *headers_json = row[2] ? row[2] : "[]";
        const char *data_json = row[3] ? row[3] : "[]";

        if (strlen(data_json) <= 2 && strlen(headers_json) <= 2) {
            skipped++;
            continue;  // empty sheet
        }

        if (mongo.UpsertSheetCells(id, user_id, headers_json, data_json)) {
            migrated++;
            if (migrated % 100 == 0)
                printf("[Migrate] Progress: %d/%d\n", migrated, total);
        } else {
            fprintf(stderr, "[Migrate] Failed for sheet %lld\n", (long long)id);
        }
    }

    mysql_free_result(result);
    mysql_close(conn);

    printf("[Migrate] Done. Migrated: %d, Skipped (empty): %d, Total: %d\n", migrated, skipped, total);
    return 0;
}

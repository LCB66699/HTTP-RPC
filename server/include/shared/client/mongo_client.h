#pragma once
#include <cstdint>
#include <memory>
#include <string>

namespace mongocxx {
class client;
class collection;
}  // namespace mongocxx

class MongoClient {
   public:
    MongoClient(const std::string &uri, const std::string &database);
    ~MongoClient();

    bool Connect();

    bool UpsertSheetCells(int64_t sheet_id, int64_t user_id,
                          const std::string &headers_json,
                          const std::string &data_json);
    bool GetSheetCells(int64_t sheet_id, std::string &headers_json,
                       std::string &data_json);
    bool DeleteSheetCells(int64_t sheet_id);

   private:
    std::string uri_;
    std::string database_;
    std::unique_ptr<mongocxx::client> client_;
    std::unique_ptr<mongocxx::collection> sheet_cells_;
};

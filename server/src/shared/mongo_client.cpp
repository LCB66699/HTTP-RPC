#include "shared/client/mongo_client.h"

#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/json.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/instance.hpp>
#include <mongocxx/uri.hpp>

#include <cstdio>
#include <nlohmann/json.hpp>

static mongocxx::instance &MongoInstance() {
    static mongocxx::instance inst;
    return inst;
}

MongoClient::MongoClient(const std::string &uri, const std::string &database)
    : uri_(uri), database_(database) {}

MongoClient::~MongoClient() {
    sheet_cells_.reset();
    client_.reset();
}

bool MongoClient::Connect() {
    try {
        MongoInstance();  // ensure driver singleton
        client_ = std::make_unique<mongocxx::client>(mongocxx::uri(uri_));
        auto db = (*client_)[database_];
        sheet_cells_ = std::make_unique<mongocxx::collection>(db["sheet_cells"]);

        // Create unique index on sheet_id
        using bsoncxx::builder::stream::document;
        using bsoncxx::builder::stream::finalize;
        auto idx_doc = document{} << "sheet_id" << 1 << finalize;
        mongocxx::options::index idx_opts{};
        idx_opts.unique(true);
        sheet_cells_->create_index(idx_doc.view(), idx_opts);

        printf("[Mongo] Connected to %s, db=%s\n", uri_.c_str(), database_.c_str());
        return true;
    } catch (const std::exception &e) {
        fprintf(stderr, "[Mongo] Connect failed: %s\n", e.what());
        return false;
    }
}

bool MongoClient::UpsertSheetCells(int64_t sheet_id, int64_t user_id,
                                   const std::string &headers_json,
                                   const std::string &data_json) {
    if (!sheet_cells_) return false;
    try {
        using bsoncxx::builder::stream::document;
        using bsoncxx::builder::stream::finalize;
        using bsoncxx::builder::stream::open_array;
        using bsoncxx::builder::stream::close_array;

        auto h_arr = nlohmann::json::parse(headers_json.empty() ? "[]" : headers_json);
        auto d_arr = nlohmann::json::parse(data_json.empty() ? "[]" : data_json);

        bsoncxx::builder::stream::document filter{};
        filter << "sheet_id" << sheet_id;

        bsoncxx::builder::stream::document update{};
        update << "$set" << open_document
               << "sheet_id" << sheet_id
               << "user_id" << user_id
               << "headers" << bsoncxx::from_json(h_arr.dump())
               << "cells" << bsoncxx::from_json(d_arr.dump())
               << close_document;

        mongocxx::options::update opts{};
        opts.upsert(true);
        sheet_cells_->update_one(filter.view(), update.view(), opts);
        return true;
    } catch (const std::exception &e) {
        fprintf(stderr, "[Mongo] UpsertSheetCells(%lld) failed: %s\n",
                (long long)sheet_id, e.what());
        return false;
    }
}

bool MongoClient::GetSheetCells(int64_t sheet_id, std::string &headers_json,
                                std::string &data_json) {
    if (!sheet_cells_) return false;
    try {
        using bsoncxx::builder::stream::document;
        using bsoncxx::builder::stream::finalize;

        auto filter = document{} << "sheet_id" << sheet_id << finalize;
        auto result = sheet_cells_->find_one(filter.view());
        if (!result) return false;

        auto view = result->view();

        auto h_it = view.find("headers");
        if (h_it != view.end()) {
            headers_json = bsoncxx::to_json(h_it->get_array().value);
        }
        auto c_it = view.find("cells");
        if (c_it != view.end()) {
            data_json = bsoncxx::to_json(c_it->get_array().value);
        }
        return true;
    } catch (const std::exception &e) {
        fprintf(stderr, "[Mongo] GetSheetCells(%lld) failed: %s\n",
                (long long)sheet_id, e.what());
        return false;
    }
}

bool MongoClient::DeleteSheetCells(int64_t sheet_id) {
    if (!sheet_cells_) return false;
    try {
        using bsoncxx::builder::stream::document;
        using bsoncxx::builder::stream::finalize;

        auto filter = document{} << "sheet_id" << sheet_id << finalize;
        sheet_cells_->delete_one(filter.view());
        return true;
    } catch (const std::exception &e) {
        fprintf(stderr, "[Mongo] DeleteSheetCells(%lld) failed: %s\n",
                (long long)sheet_id, e.what());
        return false;
    }
}

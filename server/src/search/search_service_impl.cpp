#include "search_service_impl.h"
#include "error_codes.h"

#include <cstdio>
#include <nlohmann/json.hpp>

#include "httplib.h"

grpc::Status SearchServiceImpl::Search(grpc::ServerContext *, const rpc::SearchRequest *req,
                                       rpc::SearchResponse *resp) {
    if (es_host_.empty()) {
        SET_ERROR(resp, "Search service not configured", rpc_error::UNAVAILABLE);
        return grpc::Status::OK;
    }
    std::string q = req->query();
    if (q.empty()) {
        SET_ERROR(resp, "query required", rpc_error::BAD_REQUEST);
        return grpc::Status::OK;
    }

    // 确定索引
    std::string indices;
    bool s_sheets = true, s_files = true;
    if (!req->scope().empty()) {
        s_sheets = req->scope().find("sheets") != std::string::npos;
        s_files = req->scope().find("files") != std::string::npos;
    }
    if (s_sheets && s_files)
        indices = "sheets_search,files_search";
    else if (s_sheets)
        indices = "sheets_search";
    else if (s_files)
        indices = "files_search";
    else {
        resp->set_total(0);
        return grpc::Status::OK;
    }

    int page = req->page() > 0 ? req->page() : 1;
    int page_size = req->page_size() > 0 ? req->page_size() : 20;

    // 构造 ES DSL
    nlohmann::json esq;
    esq["from"] = (page - 1) * page_size;
    esq["size"] = page_size;
    esq["track_total_hits"] = true;

    nlohmann::json query;
    query["bool"]["must"] = nlohmann::json::array();

    nlohmann::json mm;
    mm["multi_match"]["query"] = q;
    if (s_sheets && s_files)
        mm["multi_match"]["fields"] =
            nlohmann::json::array({"name^3", "description^2", "cell_content", "original_name^3", "content_text"});
    else if (s_sheets)
        mm["multi_match"]["fields"] = nlohmann::json::array({"name^3", "description^2", "cell_content"});
    else
        mm["multi_match"]["fields"] = nlohmann::json::array({"original_name^3", "content_text"});
    mm["multi_match"]["type"] = "best_fields";
    mm["multi_match"]["fuzziness"] = "AUTO";
    query["bool"]["must"].push_back(mm);

    if (!req->is_admin()) {
        nlohmann::json term;
        term["term"]["user_id"] = req->user_id();
        query["bool"]["filter"] = nlohmann::json::array({term});
    }
    esq["query"] = query;

    if (req->sort() == "date_desc")
        esq["sort"] = nlohmann::json::array({{{"updated_at", "desc"}}, {{"created_at", "desc"}}, {{"_score", "desc"}}});
    else if (req->sort() == "date_asc")
        esq["sort"] = nlohmann::json::array({{{"updated_at", "asc"}}, {{"created_at", "asc"}}, {{"_score", "desc"}}});

    esq["highlight"]["fields"]["*"] = {{"fragment_size", 80}, {"number_of_fragments", 3}};
    esq["highlight"]["pre_tags"] = nlohmann::json::array({"<em>"});
    esq["highlight"]["post_tags"] = nlohmann::json::array({"</em>"});

    // 调用 ES
    httplib::Client es(es_host_);
    es.set_connection_timeout(3, 0);
    es.set_read_timeout(5, 0);
    auto es_res = es.Post("/" + indices + "/_search", esq.dump(), "application/json");
    if (!es_res || es_res->status != 200) {
        SET_ERROR(resp, "Search temporarily unavailable", rpc_error::UNAVAILABLE);
        return grpc::Status::OK;
    }

    try {
        auto sr = nlohmann::json::parse(es_res->body);
        resp->set_total(sr["hits"]["total"].value("value", 0));
        resp->set_page(page);
        resp->set_page_size(page_size);
        resp->set_success(true);

        for (auto &hit : sr["hits"]["hits"]) {
            auto &src = hit["_source"];
            auto *r = resp->add_results();
            r->set_type(src.value("type", ""));
            r->set_id(std::to_string(src.value("id", 0LL)));
            r->set_score(hit.value("_score", 0.0));

            std::string t = src.value("type", "");
            if (t == "sheet") {
                r->set_name(src.value("name", ""));
                r->set_description(src.value("description", ""));
                r->set_username(src.value("username", ""));
                r->set_row_count(src.value("row_count", 0));
                r->set_col_count(src.value("col_count", 0));
                r->set_updated_at(src.value("updated_at", ""));
            } else {
                r->set_original_name(src.value("original_name", ""));
                r->set_mime_type(src.value("mime_type", ""));
                r->set_size(src.value("size", 0));
                r->set_username(src.value("username", ""));
                r->set_created_at(src.value("created_at", ""));
            }
            if (hit.contains("highlight")) {
                std::string hl;
                for (auto &[k, v] : hit["highlight"].items())
                    if (v.is_array() && !v.empty())
                        hl += v[0].get<std::string>() + " ";
                if (!hl.empty())
                    r->set_highlight(hl);
            }
        }
    } catch (const std::exception &e) {
        SET_ERROR(resp, "parse error", rpc_error::INTERNAL);
    }
    return grpc::Status::OK;
}

#include "call_logger.h"
#include "redis_client.h"
#include <sstream>
#include <ctime>

static std::string pad2(int n) {
    if (n < 10) return std::string("0") + std::to_string(n);
    return std::to_string(n);
}

static std::string pad3(int n) {
    if (n < 10) return std::string("00") + std::to_string(n);
    if (n < 100) return std::string("0") + std::to_string(n);
    return std::to_string(n);
}

CallLogger::CallLogger(size_t max_entries, RedisClient* redis)
    : max_entries_(max_entries), redis_(redis) {}

std::string CallLogger::NowString() const {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    std::tm* lt = std::localtime(&t);
    std::ostringstream oss;
    oss << (lt->tm_year + 1900) << "-" << pad2(lt->tm_mon + 1) << "-" << pad2(lt->tm_mday)
        << " " << pad2(lt->tm_hour) << ":" << pad2(lt->tm_min) << ":" << pad2(lt->tm_sec)
        << "." << pad3(static_cast<int>(ms.count()));
    return oss.str();
}

std::string CallLogger::SerializeEntry(const CallEntry& entry) const {
    rpc_json::Object obj;
    obj["id"] = rpc_json::Value(static_cast<double>(entry.id));
    obj["timestamp"] = rpc_json::Value(entry.timestamp);
    obj["username"] = rpc_json::Value(entry.username);
    obj["service"] = rpc_json::Value(entry.service);
    obj["method"] = rpc_json::Value(entry.method);
    obj["params"] = entry.params;
    obj["result"] = entry.result;
    obj["success"] = rpc_json::Value(entry.success);
    obj["duration_us"] = rpc_json::Value(static_cast<double>(entry.duration_us));
    return rpc_json::Value(obj).dump();
}

void CallLogger::Log(const std::string& username, const std::string& service,
                     const std::string& method, const json& params, const json& result,
                     bool success, int64_t duration_us) {
    std::lock_guard<std::mutex> lock(mtx_);
    CallEntry entry;
    entry.id = next_id_++;
    entry.timestamp = NowString();
    entry.username = username;
    entry.service = service;
    entry.method = method;
    entry.params = params;
    entry.result = result;
    entry.success = success;
    entry.duration_us = duration_us;
    entries_.push_back(std::move(entry));
    while (entries_.size() > max_entries_) entries_.pop_front();

    if (redis_ && redis_->IsConnected()) {
        redis_->PushCallEntry(SerializeEntry(entries_.back()), entries_.back().username);
    }
}

std::vector<CallEntry> CallLogger::GetHistory(int limit, int offset,
                                               const std::string& service_filter,
                                               const std::string& method_filter) const {
    std::lock_guard<std::mutex> lock(mtx_);
    std::vector<CallEntry> result;
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
        if (!service_filter.empty() && it->service != service_filter) continue;
        if (!method_filter.empty() && it->method != method_filter) continue;
        result.push_back(*it);
    }
    if (offset > 0) {
        if (offset > (int)result.size()) offset = (int)result.size();
        result.erase(result.begin(), result.begin() + offset);
    }
    if (limit > 0 && (int)result.size() > limit) {
        result.resize(limit);
    }
    return result;
}

size_t CallLogger::TotalCount() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return entries_.size();
}

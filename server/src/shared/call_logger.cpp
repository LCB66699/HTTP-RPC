#include "call_logger.h"

#include <cstdio>
#include <ctime>
#include <sstream>

#include "redis_client.h"

static std::string pad2(int n) {
    if (n < 10)
        return std::string("0") + std::to_string(n);
    return std::to_string(n);
}
static std::string pad3(int n) {
    if (n < 10)
        return std::string("00") + std::to_string(n);
    if (n < 100)
        return std::string("0") + std::to_string(n);
    return std::to_string(n);
}

CallLogger::CallLogger(size_t max_entries, RedisClient *redis) : max_entries_(max_entries), redis_(redis) {
    running_ = true;
    flush_thread_ = std::thread(&CallLogger::FlushLoop, this);
    printf("[CallLogger] background flush thread started\n");
}

CallLogger::~CallLogger() {
    running_ = false;
    cv_.notify_one();
    if (flush_thread_.joinable())
        flush_thread_.join();
}

std::string CallLogger::NowString() const {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::tm *lt = std::localtime(&t);
    std::ostringstream oss;
    oss << (lt->tm_year + 1900) << "-" << pad2(lt->tm_mon + 1) << "-" << pad2(lt->tm_mday) << " " << pad2(lt->tm_hour)
        << ":" << pad2(lt->tm_min) << ":" << pad2(lt->tm_sec) << "." << pad3(static_cast<int>(ms.count()));
    return oss.str();
}

std::string CallLogger::SerializeEntry(const CallEntry &entry) {
    json obj;
    obj["id"] = entry.id;
    obj["timestamp"] = entry.timestamp;
    obj["username"] = entry.username;
    obj["service"] = entry.service;
    obj["method"] = entry.method;
    obj["params"] = entry.params;
    obj["result"] = entry.result;
    obj["success"] = entry.success;
    obj["duration_us"] = entry.duration_us;
    return obj.dump();
}

void CallLogger::Log(const std::string &username, const std::string &service, const std::string &method,
                     const json &params, const json &result, bool success, int64_t duration_us) {
    std::string serialized;
    {
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
        while (entries_.size() > max_entries_)
            entries_.pop_front();
        serialized = SerializeEntry(entries_.back());
        pending_.emplace_back(std::move(serialized), entries_.back().username);
    }  // ← 解锁 — Redis I/O 不在此处
    cv_.notify_one();
}

void CallLogger::FlushLoop() {
    using Entry = std::pair<std::string, std::string>;
    while (running_) {
        std::vector<Entry> batch;
        {
            std::unique_lock<std::mutex> lock(mtx_);
            cv_.wait_for(lock, std::chrono::milliseconds(100), [this] { return !pending_.empty() || !running_; });
            if (!running_ && pending_.empty())
                break;
            batch.swap(pending_);
        }

        if (redis_ && redis_->IsConnected()) {
            redis_->BatchPushCallEntries(batch);
        }
    }
    // 退出前清空剩余
    if (redis_ && redis_->IsConnected() && !pending_.empty()) {
        redis_->BatchPushCallEntries(pending_);
    }
}

std::vector<CallEntry> CallLogger::GetHistory(int limit, int offset, const std::string &service_filter,
                                              const std::string &method_filter) const {
    std::lock_guard<std::mutex> lock(mtx_);
    std::vector<CallEntry> result;
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
        if (!service_filter.empty() && it->service != service_filter)
            continue;
        if (!method_filter.empty() && it->method != method_filter)
            continue;
        result.push_back(*it);
    }
    if (offset > 0) {
        if (offset > (int)result.size())
            offset = (int)result.size();
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

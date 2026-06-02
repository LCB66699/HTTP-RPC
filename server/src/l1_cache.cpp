#include "l1_cache.h"
#include <algorithm>

L1Cache::L1Cache(size_t max_entries, int ttl_seconds)
    : max_entries_(max_entries), ttl_seconds_(ttl_seconds) {}

std::optional<std::string> L1Cache::Get(const std::string& key) {
    std::unique_lock<std::shared_mutex> lock(mtx_);
    auto it = index_.find(key);
    if (it == index_.end()) { misses_++; return std::nullopt; }

    auto now = std::chrono::steady_clock::now();
    if (it->second->expires_at < now) {
        lru_.erase(it->second);
        index_.erase(it);
        misses_++;
        return std::nullopt;
    }

    // Move to front (LRU promotion)
    lru_.splice(lru_.begin(), lru_, it->second);
    hits_++;
    return it->second->value;
}

void L1Cache::Set(const std::string& key, const std::string& value) {
    std::unique_lock<std::shared_mutex> lock(mtx_);

    auto it = index_.find(key);
    if (it != index_.end()) {
        // Update existing — move to front
        it->second->value = value;
        it->second->expires_at = std::chrono::steady_clock::now()
            + std::chrono::seconds(ttl_seconds_);
        lru_.splice(lru_.begin(), lru_, it->second);
        return;
    }

    if (lru_.size() >= max_entries_) {
        // Evict oldest (LRU tail) + expired
        EvictExpired();
        if (lru_.size() >= max_entries_) EvictLru();
    }

    lru_.emplace_front(Entry{key, value,
        std::chrono::steady_clock::now() + std::chrono::seconds(ttl_seconds_)});
    index_[key] = lru_.begin();
}

void L1Cache::Delete(const std::string& key) {
    std::unique_lock<std::shared_mutex> lock(mtx_);
    auto it = index_.find(key);
    if (it != index_.end()) {
        lru_.erase(it->second);
        index_.erase(it);
    }
}

void L1Cache::Clear() {
    std::unique_lock<std::shared_mutex> lock(mtx_);
    lru_.clear();
    index_.clear();
}

void L1Cache::EvictExpired() {
    auto now = std::chrono::steady_clock::now();
    while (!lru_.empty() && lru_.back().expires_at < now) {
        index_.erase(lru_.back().key);
        lru_.pop_back();
    }
}

void L1Cache::EvictLru() {
    if (!lru_.empty()) {
        index_.erase(lru_.back().key);
        lru_.pop_back();
    }
}

size_t L1Cache::Size() const {
    std::shared_lock<std::shared_mutex> lock(mtx_);
    return lru_.size();
}

size_t L1Cache::HitCount() const { return hits_; }
size_t L1Cache::MissCount() const { return misses_; }

// L1 in-process LRU cache with TTL — sits before Redis (L2) and MySQL.
// Thread-safe via shared_mutex. Canal-driven Pub/Sub invalidates entries
// before TTL expires, giving near-real-time consistency.
//
// Usage:
//   L1Cache cache(10000, 30);  // 10k entries, 30s TTL
//   cache.Set("key", value);
//   auto v = cache.Get<std::string>("key");  // returns std::optional
//   cache.Delete("key");
#pragma once
#include <chrono>
#include <functional>
#include <list>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>

class L1Cache {
public:
  L1Cache(size_t max_entries = 10000, int ttl_seconds = 30);

  std::optional<std::string> Get(const std::string &key);
  void Set(const std::string &key, const std::string &value);
  void Delete(const std::string &key);
  void Clear();

  // Stats
  size_t Size() const;
  size_t HitCount() const;
  size_t MissCount() const;

private:
  struct Entry {
    std::string key;
    std::string value;
    std::chrono::steady_clock::time_point expires_at;
  };
  using Iterator = std::list<Entry>::iterator;

  void EvictExpired();
  void EvictLru();

  mutable std::shared_mutex mtx_;
  std::list<Entry> lru_;
  std::unordered_map<std::string, Iterator> index_;
  size_t max_entries_;
  int ttl_seconds_;
  mutable size_t hits_{0};
  mutable size_t misses_{0};
};

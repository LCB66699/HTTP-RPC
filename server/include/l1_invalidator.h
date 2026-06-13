// Listens on Redis Pub/Sub channel "cache:invalidate".
// Canal Adapter publishes "u:5:sheet:123" on this channel after binlog DELETE.
// Each service process subscribes and evicts the matching L1 entry.
#pragma once
#include <atomic>
#include <string>
#include <thread>

class L1Cache;
class RedisClient;

class L1CacheInvalidator {
public:
  L1CacheInvalidator(L1Cache *cache, RedisClient *redis);
  ~L1CacheInvalidator();

  void Start();
  void Stop();

private:
  void SubscribeLoop();

  L1Cache *cache_;
  RedisClient *redis_;
  std::thread sub_thread_;
  std::atomic<bool> running_{true};
};

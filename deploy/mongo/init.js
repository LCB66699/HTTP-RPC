// MongoDB 初始化 — 创建 rpc_search 数据库的集合和索引

db = db.getSiblingDB('rpc_search');

// doc_contents: 文件解析后的文本内容
db.createCollection('doc_contents');
db.doc_contents.createIndex({ file_id: 1 }, { unique: true });
db.doc_contents.createIndex({ user_id: 1 });
db.doc_contents.createIndex({ updated_at: -1 });

// sheet_contents: 表格全文数据快照
db.createCollection('sheet_contents');
db.sheet_contents.createIndex({ sheet_id: 1 }, { unique: true });
db.sheet_contents.createIndex({ user_id: 1 });
db.sheet_contents.createIndex({ updated_at: -1 });

// search_sync_log: 同步日志（调试/回溯用）
db.createCollection('search_sync_log');
db.search_sync_log.createIndex({ entity_type: 1, entity_id: 1 });
db.search_sync_log.createIndex({ synced_at: -1 });
// TTL 索引，30 天后自动清理
db.search_sync_log.createIndex({ synced_at: 1 }, { expireAfterSeconds: 2592000 });

print('[mongo-init] rpc_search database initialized.');

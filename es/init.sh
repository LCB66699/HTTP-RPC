#!/bin/sh
# ES 索引初始化 — 创建 sheets_search 和 files_search 索引映射
# 等待 ES 完全就绪

ES="http://elasticsearch:9200"

echo "[es-init] Waiting for Elasticsearch..."
until curl -s "$ES/_cluster/health" | grep -qE '"status":"(green|yellow)"'; do
  sleep 2
done
echo "[es-init] Elasticsearch is ready."

# ---- sheets_search ----
echo "[es-init] Creating index: sheets_search"
curl -s -X PUT "$ES/sheets_search" -H 'Content-Type: application/json' -d '{
  "settings": {
    "number_of_shards": 2,
    "number_of_replicas": 1,
    "analysis": {
      "analyzer": {
        "ik_smart_analyzer": { "type": "custom", "tokenizer": "ik_smart" },
        "ik_max_analyzer":  { "type": "custom", "tokenizer": "ik_max_word" }
      }
    }
  },
  "mappings": {
    "properties": {
      "id":          { "type": "long" },
      "user_id":     { "type": "long" },
      "username":    { "type": "keyword" },
      "name":        { "type": "text", "analyzer": "ik_max_word", "search_analyzer": "ik_smart" },
      "description": { "type": "text", "analyzer": "ik_max_word", "search_analyzer": "ik_smart" },
      "cell_content":{ "type": "text", "analyzer": "ik_max_word", "search_analyzer": "ik_smart" },
      "row_count":   { "type": "integer" },
      "col_count":   { "type": "integer" },
      "created_at":  { "type": "date" },
      "updated_at":  { "type": "date" },
      "type":        { "type": "keyword" }
    }
  }
}'
echo ""

# ---- files_search ----
echo "[es-init] Creating index: files_search"
curl -s -X PUT "$ES/files_search" -H 'Content-Type: application/json' -d '{
  "settings": {
    "number_of_shards": 2,
    "number_of_replicas": 1,
    "analysis": {
      "analyzer": {
        "ik_smart_analyzer": { "type": "custom", "tokenizer": "ik_smart" },
        "ik_max_analyzer":  { "type": "custom", "tokenizer": "ik_max_word" }
      }
    }
  },
  "mappings": {
    "properties": {
      "id":            { "type": "long" },
      "user_id":       { "type": "long" },
      "username":      { "type": "keyword" },
      "original_name": { "type": "text", "analyzer": "ik_max_word", "search_analyzer": "ik_smart" },
      "mime_type":     { "type": "keyword" },
      "size":          { "type": "long" },
      "content_text":  { "type": "text", "analyzer": "ik_max_word", "search_analyzer": "ik_smart" },
      "created_at":    { "type": "date" },
      "type":          { "type": "keyword" }
    }
  }
}'
echo ""

echo "[es-init] Done — indices created."

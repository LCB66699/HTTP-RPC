// Outbox 兜底 — CAS 状态机轮询 MySQL outbox 表，补偿投递事件
//
// 状态流转:
//   0(pending) → 1(claimed, 补偿器正在处理) → 2(published, 已成功投递)
//                                              → 0(发送失败, 下次重试)
//   attempts >= 5 不再处理
//
// 分片配置通过环境变量:
//   OUTBOX_SHEET_HOSTS=mysql-spreadsheet-0:rpc_spreadsheet,mysql-spreadsheet-1:rpc_spreadsheet_1
//   OUTBOX_FILE_HOSTS=mysql-file-0:rpc_file,mysql-file-1:rpc_file_1
//   兼容旧配置: CANAL_MYSQL_HOST=mysql-spreadsheet-0

package main

import (
	"database/sql"
	"fmt"
	"log"
	"strings"
	"time"

	_ "github.com/go-sql-driver/mysql"
	amqp "github.com/rabbitmq/amqp091-go"
)

const maxOutboxAttempts = 5

// rabbitPublish is a package-level variable for testability (mocked in tests).
var rabbitPublish = func(ch *amqp.Channel, eventType string, headers amqp.Table, body []byte) error {
	return ch.Publish("rpc.events", eventType, false, false,
		amqp.Publishing{ContentType: "application/json", Body: body, Headers: headers})
}

// ---- startup ----

func startOutboxPoller(ch *amqp.Channel, redisAddr, redisPass string) {
	user := getenv("CANAL_MYSQL_USER", "root")
	pass := getenv("MYSQL_ROOT_PASSWORD", "123456")
	port := getenv("CANAL_MYSQL_PORT", "3306")

	// 兼容旧配置: 只设 CANAL_MYSQL_HOST 时退化为旧行为
	legacyHost := getenv("CANAL_MYSQL_HOST", "mysql-spreadsheet-0")

	sheetHosts := getenv("OUTBOX_SHEET_HOSTS", legacyHost+":rpc_spreadsheet")
	for _, p := range parseHostPairs(sheetHosts, port, user, pass) {
		go pollSingleMySQL(p.dsn, p.label, ch, redisAddr, redisPass)
	}

	fileHosts := getenv("OUTBOX_FILE_HOSTS", "mysql-file-0:rpc_file")
	for _, p := range parseHostPairs(fileHosts, port, user, pass) {
		go pollSingleMySQL(p.dsn, p.label, ch, redisAddr, redisPass)
	}
}

type hostPair struct {
	dsn   string
	label string
}

// parseHostPairs 解析 "host1:db1,host2:db2" 格式的分片列表
func parseHostPairs(input, port, user, pass string) []hostPair {
	var pairs []hostPair
	for _, entry := range strings.Split(input, ",") {
		entry = strings.TrimSpace(entry)
		if entry == "" {
			continue
		}
		parts := strings.SplitN(entry, ":", 2)
		host := parts[0]
		dbName := "rpc_spreadsheet"
		if len(parts) >= 2 {
			dbName = parts[1]
		}
		dsn := fmt.Sprintf("%s:%s@tcp(%s:%s)/%s", user, pass, host, port, dbName)
		pairs = append(pairs, hostPair{dsn: dsn, label: host + "/" + dbName})
	}
	return pairs
}

// ---- per-shard poll loop ----

func pollSingleMySQL(dsn, label string, ch *amqp.Channel, redisAddr, redisPass string) {
	log.Printf("[Outbox] connecting %s", label)
	db, err := sql.Open("mysql", dsn)
	if err != nil {
		log.Printf("[Outbox] %s open failed: %v", label, err)
		return
	}
	defer db.Close()
	db.SetMaxOpenConns(2)
	db.SetMaxIdleConns(1)

	// 等待数据库就绪
	for i := 0; i < 30; i++ {
		if err := db.Ping(); err == nil {
			break
		}
		time.Sleep(2 * time.Second)
	}

	for {
		pollOutboxCAS(db, label, ch, redisAddr, redisPass)
		time.Sleep(2 * time.Second)
	}
}

type pendingRow struct {
	id           int64
	eventType    string
	payload      string
	traceContext string
}

// ---- CAS 状态机 ----

func pollOutboxCAS(db *sql.DB, label string, ch *amqp.Channel, redisAddr, redisPass string) {
	// Step 1: 事务内 SELECT FOR UPDATE 锁定 pending 行
	tx, err := db.Begin()
	if err != nil {
		log.Printf("[Outbox] %s begin tx: %v", label, err)
		return
	}
	defer tx.Rollback() // Commit 后 Rollback 是 no-op，安全

	rows, err := tx.Query(
		`SELECT id, event_type, payload, COALESCE(trace_context,'')
		 FROM outbox
		 WHERE status=0 AND attempts < ?
		 ORDER BY id LIMIT 50 FOR UPDATE`, maxOutboxAttempts)
	if err != nil {
		log.Printf("[Outbox] %s query: %v", label, err)
		return
	}

	var pending []pendingRow
	for rows.Next() {
		var r pendingRow
		if err := rows.Scan(&r.id, &r.eventType, &r.payload, &r.traceContext); err != nil {
			log.Printf("[Outbox] %s scan: %v", label, err)
			continue
		}
		pending = append(pending, r)
	}
	rows.Close()

	if err := rows.Err(); err != nil {
		log.Printf("[Outbox] %s rows iteration: %v", label, err)
		return
	}

	if len(pending) == 0 {
		return
	}

	// Step 2: 标记为 claimed (status=1)，防止多实例重复补偿
	for _, r := range pending {
		if _, err := tx.Exec("UPDATE outbox SET status=1, attempts=attempts+1 WHERE id=?", r.id); err != nil {
			log.Printf("[Outbox] %s claim id=%d: %v", label, r.id, err)
		}
	}

	if err := tx.Commit(); err != nil {
		log.Printf("[Outbox] %s commit: %v", label, err)
		return
	}

	// Step 3: 逐个投递（不在事务内，避免长事务阻塞）
	for _, r := range pending {
		var pubErr error
		if strings.HasPrefix(r.eventType, "cache:") {
			// Cache invalidation → Redis Pub/Sub
			pubErr = redisPublish(redisAddr, redisPass, r.eventType, r.payload)
		} else {
			// 普通事件 → RabbitMQ
			headers := amqp.Table{}
			if r.traceContext != "" {
				headers["traceparent"] = r.traceContext
			}
			pubErr = rabbitPublish(ch, r.eventType, headers, []byte(r.payload))
		}

		if pubErr != nil {
			log.Printf("[Outbox] %s publish id=%d type=%s: %v", label, r.id, r.eventType, pubErr)
			// 回滚: 放回 pending，下次重试
			db.Exec("UPDATE outbox SET status=0, last_error=? WHERE id=?", pubErr.Error(), r.id)
			continue
		}

		// 成功: 标记为已发送
		db.Exec("UPDATE outbox SET status=2, published_at=NOW() WHERE id=?", r.id)
		log.Printf("[Outbox] %s published id=%d type=%s", label, r.id, r.eventType)
	}

	// Step 4: 清理超过 1 天的已确认记录
	if res, err := db.Exec("DELETE FROM outbox WHERE status=2 AND published_at < NOW() - INTERVAL 1 DAY"); err == nil {
		if n, _ := res.RowsAffected(); n > 0 {
			log.Printf("[Outbox] %s cleaned %d old records", label, n)
		}
	}
}

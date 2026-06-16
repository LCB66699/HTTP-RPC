// Outbox 兜底 — 轮询 MySQL outbox 表，补发 RabbitMQ 事件
package main

import (
	"database/sql"
	"fmt"
	"log"
	"time"

	_ "github.com/go-sql-driver/mysql"
	amqp "github.com/rabbitmq/amqp091-go"
)

func startOutboxPoller(ch *amqp.Channel) {
	// 连接两个 MySQL（spreadsheet + file），每个实例有自己的 outbox 表
	mysqls := []string{
		fmt.Sprintf("%s:%s@tcp(%s:%d)/%s",
			getenv("CANAL_MYSQL_USER", "root"),
			getenv("MYSQL_ROOT_PASSWORD", "123456"),
			getenv("CANAL_MYSQL_HOST", "mysql-spreadsheet-0"),
			3306, "rpc_spreadsheet"),
		fmt.Sprintf("%s:%s@tcp(%s:%d)/%s",
			getenv("CANAL_MYSQL_USER", "root"),
			getenv("MYSQL_ROOT_PASSWORD", "123456"),
			"mysql-file-0",
			3306, "rpc_file"),
	}

	for _, dsn := range mysqls {
		go pollSingleMySQL(dsn, ch)
	}
}

func pollSingleMySQL(dsn string, ch *amqp.Channel) {
	log.Printf("[Outbox] Connecting to dsn...")
	db, err := sql.Open("mysql", dsn)
	if err != nil {
		log.Printf("[Outbox] open failed: %v", err)
		return
	}
	defer db.Close()
	db.SetMaxOpenConns(2)
	db.SetMaxIdleConns(1)

	for {
		pollOutbox(db, ch)
		time.Sleep(2 * time.Second)
	}
}

func pollOutbox(db *sql.DB, ch *amqp.Channel) {
	rows, err := db.Query("SELECT id, event_type, payload, trace_context FROM outbox ORDER BY id LIMIT 100")
	if err != nil {
		if err != sql.ErrNoRows {
			log.Printf("[Outbox] query error: %v", err)
		}
		return
	}
	defer rows.Close()

	var maxID int64
	for rows.Next() {
		var id int64
		var eventType, payload string
		var traceContext sql.NullString
		if err := rows.Scan(&id, &eventType, &payload, &traceContext); err != nil {
			continue
		}
		headers := amqp.Table{}
		if traceContext.Valid && traceContext.String != "" {
			headers["traceparent"] = traceContext.String
		}
		if err := ch.Publish("rpc.events", eventType, false, false,
			amqp.Publishing{
				ContentType: "application/json",
				Body:        []byte(payload),
				Headers:     headers,
			}); err != nil {
			log.Printf("[Outbox] publish error: %v", err)
			return
		}
		maxID = id
	}

	if maxID > 0 {
		db.Exec("DELETE FROM outbox WHERE id <= ?", maxID)
		log.Printf("[Outbox] flushed up to id=%d", maxID)
	}
}

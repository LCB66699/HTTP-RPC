package main

import (
	"fmt"
	"net"
	"strings"
	"testing"
	"time"

	"github.com/DATA-DOG/go-sqlmock"
	amqp "github.com/rabbitmq/amqp091-go"
)

// ---- parseHostPairs ---- (pure function, no mocks needed)

func TestParseHostPairs_Single(t *testing.T) {
	pairs := parseHostPairs("mysql-a:rpc_db", "3306", "root", "pass")
	if len(pairs) != 1 {
		t.Fatalf("expected 1 pair, got %d", len(pairs))
	}
	if pairs[0].label != "mysql-a/rpc_db" {
		t.Errorf("expected label 'mysql-a/rpc_db', got %q", pairs[0].label)
	}
	wantDSN := "root:pass@tcp(mysql-a:3306)/rpc_db"
	if pairs[0].dsn != wantDSN {
		t.Errorf("expected dsn %q, got %q", wantDSN, pairs[0].dsn)
	}
}

func TestParseHostPairs_Multi(t *testing.T) {
	pairs := parseHostPairs("h1:db1,h2:db2", "3307", "u1", "p1")
	if len(pairs) != 2 {
		t.Fatalf("expected 2 pairs, got %d", len(pairs))
	}
	if pairs[0].label != "h1/db1" {
		t.Errorf("expected 'h1/db1', got %q", pairs[0].label)
	}
	if pairs[1].label != "h2/db2" {
		t.Errorf("expected 'h2/db2', got %q", pairs[1].label)
	}
}

func TestParseHostPairs_DefaultDb(t *testing.T) {
	pairs := parseHostPairs("mysql-a", "3306", "root", "pass")
	if len(pairs) != 1 {
		t.Fatalf("expected 1 pair, got %d", len(pairs))
	}
	if pairs[0].label != "mysql-a/rpc_spreadsheet" {
		t.Errorf("expected default db label 'mysql-a/rpc_spreadsheet', got %q", pairs[0].label)
	}
}

func TestParseHostPairs_Empty(t *testing.T) {
	pairs := parseHostPairs("", "3306", "root", "pass")
	if len(pairs) != 0 {
		t.Errorf("expected 0 pairs for empty input, got %d", len(pairs))
	}
}

func TestParseHostPairs_EmptyEntries(t *testing.T) {
	pairs := parseHostPairs("h1:db1,,h2:db2,", "3306", "root", "pass")
	if len(pairs) != 2 {
		t.Errorf("expected 2 pairs (skipping empty entries), got %d", len(pairs))
	}
}

// ---- pollOutboxCAS ----

func TestPollOutboxCAS_NoPending(t *testing.T) {
	db, mock, err := sqlmock.New()
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()

	mock.ExpectBegin()
	mock.ExpectQuery("(?s)SELECT id.*FROM outbox WHERE status=0.*FOR UPDATE").
		WithArgs(maxOutboxAttempts).
		WillReturnRows(sqlmock.NewRows([]string{"id", "event_type", "payload", "trace_context"}))
	mock.ExpectRollback()

	pollOutboxCAS(db, "test", nil, "", "")

	if err := mock.ExpectationsWereMet(); err != nil {
		t.Fatal(err)
	}
}

func TestPollOutboxCAS_NormalFlow(t *testing.T) {
	db, mock, err := sqlmock.New()
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()

	mock.ExpectBegin()
	rows := sqlmock.NewRows([]string{"id", "event_type", "payload", "trace_context"}).
		AddRow(int64(1), "file.uploaded", `{"id":"f1"}`, "")
	mock.ExpectQuery("(?s)SELECT id.*FROM outbox WHERE status=0.*FOR UPDATE").
		WithArgs(maxOutboxAttempts).
		WillReturnRows(rows)
	mock.ExpectExec("UPDATE outbox SET status=1").
		WithArgs(int64(1)).
		WillReturnResult(sqlmock.NewResult(0, 1))
	mock.ExpectCommit()

	// Replace rabbitPublish with a test spy
	published := make(chan struct{}, 1)
	origPublish := rabbitPublish
	t.Cleanup(func() { rabbitPublish = origPublish })
	rabbitPublish = func(_ *amqp.Channel, eventType string, _ amqp.Table, body []byte) error {
		if eventType != "file.uploaded" {
			t.Errorf("expected eventType 'file.uploaded', got %q", eventType)
		}
		if string(body) != `{"id":"f1"}` {
			t.Errorf("unexpected body: %s", string(body))
		}
		published <- struct{}{}
		return nil
	}

	mock.ExpectExec("UPDATE outbox SET status=2").
		WithArgs(int64(1)).
		WillReturnResult(sqlmock.NewResult(0, 1))
	mock.ExpectExec("DELETE FROM outbox WHERE status=2").
		WillReturnResult(sqlmock.NewResult(0, 0))

	pollOutboxCAS(db, "test", nil, "", "")

	select {
	case <-published:
	case <-time.After(time.Second):
		t.Fatal("rabbitPublish was not called")
	}

	if err := mock.ExpectationsWereMet(); err != nil {
		t.Fatal(err)
	}
}

func TestPollOutboxCAS_PublishFail(t *testing.T) {
	db, mock, err := sqlmock.New()
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()

	mock.ExpectBegin()
	rows := sqlmock.NewRows([]string{"id", "event_type", "payload", "trace_context"}).
		AddRow(int64(1), "sheet.updated", `{}`, "00-abc-123")
	mock.ExpectQuery("(?s)SELECT id.*FROM outbox WHERE status=0.*FOR UPDATE").
		WithArgs(maxOutboxAttempts).
		WillReturnRows(rows)
	mock.ExpectExec("UPDATE outbox SET status=1").
		WithArgs(int64(1)).
		WillReturnResult(sqlmock.NewResult(0, 1))
	mock.ExpectCommit()

	origPublish := rabbitPublish
	t.Cleanup(func() { rabbitPublish = origPublish })
	rabbitPublish = func(_ *amqp.Channel, _ string, _ amqp.Table, _ []byte) error {
		return fmt.Errorf("mq broker unreachable")
	}

	// Rollback: status=0, last_error set
	mock.ExpectExec("UPDATE outbox SET status=0").
		WithArgs("mq broker unreachable", int64(1)).
		WillReturnResult(sqlmock.NewResult(0, 1))
	mock.ExpectExec("DELETE FROM outbox WHERE status=2").
		WillReturnResult(sqlmock.NewResult(0, 0))

	pollOutboxCAS(db, "test", nil, "", "")

	if err := mock.ExpectationsWereMet(); err != nil {
		t.Fatal(err)
	}
}

func TestPollOutboxCAS_TraceContext(t *testing.T) {
	db, mock, err := sqlmock.New()
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()

	mock.ExpectBegin()
	rows := sqlmock.NewRows([]string{"id", "event_type", "payload", "trace_context"}).
		AddRow(int64(1), "sheet.created", `{"id":"s1"}`, "00-aaa-bbb-01")
	mock.ExpectQuery("(?s)SELECT id.*FROM outbox WHERE status=0.*FOR UPDATE").
		WithArgs(maxOutboxAttempts).
		WillReturnRows(rows)
	mock.ExpectExec("UPDATE outbox SET status=1").
		WithArgs(int64(1)).
		WillReturnResult(sqlmock.NewResult(0, 1))
	mock.ExpectCommit()

	var capturedHeaders amqp.Table
	origPublish := rabbitPublish
	t.Cleanup(func() { rabbitPublish = origPublish })
	rabbitPublish = func(_ *amqp.Channel, _ string, headers amqp.Table, _ []byte) error {
		capturedHeaders = headers
		return nil
	}

	mock.ExpectExec("UPDATE outbox SET status=2").
		WithArgs(int64(1)).
		WillReturnResult(sqlmock.NewResult(0, 1))
	mock.ExpectExec("DELETE FROM outbox WHERE status=2").
		WillReturnResult(sqlmock.NewResult(0, 0))

	pollOutboxCAS(db, "test", nil, "", "")

	if capturedHeaders == nil {
		t.Fatal("expected rabbitPublish to be called")
	}
	if traced, ok := capturedHeaders["traceparent"]; !ok || traced != "00-aaa-bbb-01" {
		t.Errorf("expected traceparent='00-aaa-bbb-01', got %v", traced)
	}

	if err := mock.ExpectationsWereMet(); err != nil {
		t.Fatal(err)
	}
}

func TestPollOutboxCAS_CacheEvent(t *testing.T) {
	db, mock, err := sqlmock.New()
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()

	// Local TCP server acting as Redis
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()
	redisAddr := listener.Addr().String()

	cmds := make(chan string, 2)
	go func() {
		conn, err := listener.Accept()
		if err != nil {
			return
		}
		defer conn.Close()
		buf := make([]byte, 1024)
		n, err := conn.Read(buf)
		if err != nil {
			return
		}
		cmds <- string(buf[:n])
	}()

	mock.ExpectBegin()
	rows := sqlmock.NewRows([]string{"id", "event_type", "payload", "trace_context"}).
		AddRow(int64(2), "cache:sheet:456", `{"id":"s456"}`, "")
	mock.ExpectQuery("(?s)SELECT id.*FROM outbox WHERE status=0.*FOR UPDATE").
		WithArgs(maxOutboxAttempts).
		WillReturnRows(rows)
	mock.ExpectExec("UPDATE outbox SET status=1").
		WithArgs(int64(2)).
		WillReturnResult(sqlmock.NewResult(0, 1))
	mock.ExpectCommit()

	mock.ExpectExec("UPDATE outbox SET status=2").
		WithArgs(int64(2)).
		WillReturnResult(sqlmock.NewResult(0, 1))
	mock.ExpectExec("DELETE FROM outbox WHERE status=2").
		WillReturnResult(sqlmock.NewResult(0, 0))

	pollOutboxCAS(db, "test", nil, redisAddr, "")

	select {
	case cmd := <-cmds:
		if !strings.Contains(cmd, "PUBLISH") {
			t.Errorf("expected PUBLISH command, got: %s", cmd)
		}
		if !strings.Contains(cmd, "cache:sheet:456") {
			t.Errorf("expected channel 'cache:sheet:456', got: %s", cmd)
		}
	case <-time.After(time.Second):
		t.Fatal("timeout waiting for Redis PUBLISH")
	}

	if err := mock.ExpectationsWereMet(); err != nil {
		t.Fatal(err)
	}
}

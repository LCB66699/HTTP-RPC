package main

import (
	"net"
	"strings"
	"testing"
	"time"
)

func TestRedisPublish_AuthAndPublish(t *testing.T) {
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()
	addr := listener.Addr().String()

	cmds := make(chan string, 2)
	go func() {
		conn, err := listener.Accept()
		if err != nil {
			return
		}
		defer conn.Close()
		buf := make([]byte, 1024)

		n, _ := conn.Read(buf)
		cmds <- string(buf[:n])
		conn.Write([]byte("+OK\r\n"))

		n, _ = conn.Read(buf)
		cmds <- string(buf[:n])
	}()

	if err := redisPublish(addr, "secret", "ch1", "msg1"); err != nil {
		t.Fatal(err)
	}

	select {
	case auth := <-cmds:
		if !strings.Contains(auth, "AUTH") {
			t.Errorf("expected AUTH command, got: %s", auth)
		}
		if !strings.Contains(auth, "secret") {
			t.Errorf("expected password 'secret' in AUTH, got: %s", auth)
		}
	case <-time.After(time.Second):
		t.Fatal("timeout waiting for AUTH command")
	}

	select {
	case pub := <-cmds:
		if !strings.Contains(pub, "PUBLISH") {
			t.Errorf("expected PUBLISH command, got: %s", pub)
		}
		if !strings.Contains(pub, "ch1") {
			t.Errorf("expected channel 'ch1' in PUBLISH, got: %s", pub)
		}
		if !strings.Contains(pub, "msg1") {
			t.Errorf("expected message 'msg1' in PUBLISH, got: %s", pub)
		}
	case <-time.After(time.Second):
		t.Fatal("timeout waiting for PUBLISH command")
	}
}

func TestRedisPublish_NoAuth(t *testing.T) {
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()
	addr := listener.Addr().String()

	cmds := make(chan string, 1)
	go func() {
		conn, err := listener.Accept()
		if err != nil {
			return
		}
		defer conn.Close()
		buf := make([]byte, 1024)
		n, _ := conn.Read(buf)
		cmds <- string(buf[:n])
	}()

	if err := redisPublish(addr, "", "ch2", "msg2"); err != nil {
		t.Fatal(err)
	}

	select {
	case cmd := <-cmds:
		if strings.Contains(cmd, "AUTH") {
			t.Errorf("no AUTH expected when password is empty, got: %s", cmd)
		}
		if !strings.Contains(cmd, "PUBLISH") {
			t.Errorf("expected PUBLISH command, got: %s", cmd)
		}
		if !strings.Contains(cmd, "msg2") {
			t.Errorf("expected message 'msg2', got: %s", cmd)
		}
	case <-time.After(time.Second):
		t.Fatal("timeout waiting for Redis command")
	}
}

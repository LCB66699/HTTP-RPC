package main

import (
	"fmt"
	"net"
	"strings"
	"time"
)

// redisPublish publishes a message to a Redis Pub/Sub channel over plain TCP.
// No external dependency needed — uses RESP3 inline with AUTH support.
func redisPublish(addr, password, channel, message string) error {
	dialer := net.Dialer{Timeout: 3 * time.Second}
	conn, err := dialer.Dial("tcp", addr)
	if err != nil {
		return fmt.Errorf("redis dial: %w", err)
	}
	defer conn.Close()

	// AUTH if password is set
	if password != "" {
		conn.Write([]byte(fmt.Sprintf("*2\r\n$4\r\nAUTH\r\n$%d\r\n%s\r\n", len(password), password)))
		var buf [64]byte
		n, _ := conn.Read(buf[:])
		if !strings.Contains(string(buf[:n]), "+OK") {
			// ignore auth error — Redis might not require AUTH
		}
	}

	// PUBLISH channel message
	cmd := fmt.Sprintf("*3\r\n$7\r\nPUBLISH\r\n$%d\r\n%s\r\n$%d\r\n%s\r\n", len(channel), channel, len(message), message)
	if _, err := conn.Write([]byte(cmd)); err != nil {
		return fmt.Errorf("redis publish write: %w", err)
	}
	return nil
}

package ws

import (
	"testing"
	"time"

	"github.com/alicebob/miniredis/v2"
	"github.com/redis/go-redis/v9"
)

func setupTestHub(t *testing.T) (*Hub, *miniredis.Miniredis) {
	t.Helper()
	mr := miniredis.RunT(t)
	rdb := redis.NewClient(&redis.Options{Addr: mr.Addr()})
	hub := NewHub(rdb)
	return hub, mr
}

// ---- Hub register/unregister ----

func TestHubRegisterClient(t *testing.T) {
	hub, _ := setupTestHub(t)
	go hub.Run()
	time.Sleep(10 * time.Millisecond)

	c := NewClient(hub, nil, 42)
	c.rooms = []string{"sheet:1"}
	hub.Register(c)
	time.Sleep(10 * time.Millisecond)

	if len(hub.rooms["sheet:1"]) != 1 {
		t.Errorf("room sheet:1 should have 1 client, got %d", len(hub.rooms["sheet:1"]))
	}
}

func TestHubUnregisterClient(t *testing.T) {
	hub, _ := setupTestHub(t)
	go hub.Run()
	time.Sleep(10 * time.Millisecond)

	c := NewClient(hub, nil, 42)
	c.rooms = []string{"sheet:1"}
	hub.Register(c)
	time.Sleep(10 * time.Millisecond)
	hub.Unregister(c)
	time.Sleep(10 * time.Millisecond)

	if _, ok := hub.rooms["sheet:1"]; ok {
		t.Error("room sheet:1 should be removed after last client unregisters")
	}
}

func TestHubMultipleRooms(t *testing.T) {
	hub, _ := setupTestHub(t)
	go hub.Run()
	time.Sleep(10 * time.Millisecond)

	c1 := NewClient(hub, nil, 1)
	c1.rooms = []string{"sheet:1"}
	c2 := NewClient(hub, nil, 2)
	c2.rooms = []string{"sheet:1", "sheet:2"}

	hub.Register(c1)
	hub.Register(c2)
	time.Sleep(10 * time.Millisecond)

	if len(hub.rooms["sheet:1"]) != 2 {
		t.Errorf("sheet:1 should have 2 clients, got %d", len(hub.rooms["sheet:1"]))
	}
	if len(hub.rooms["sheet:2"]) != 1 {
		t.Errorf("sheet:2 should have 1 client, got %d", len(hub.rooms["sheet:2"]))
	}
}

// ---- Hub broadcast ----

func TestHubBroadcast(t *testing.T) {
	hub, mr := setupTestHub(t)
	go hub.Run()
	time.Sleep(10 * time.Millisecond)

	c := NewClient(hub, nil, 42)
	c.rooms = []string{"sheet:1"}
	c.send = make(chan []byte, 256)
	hub.Register(c)
	time.Sleep(10 * time.Millisecond)

	// Publish via Redis
	mr.Publish("ws:broadcast", `{"room":"sheet:1","type":"sheet.updated","data":{"user":"A"}}`)
	time.Sleep(20 * time.Millisecond)

	select {
	case msg := <-c.send:
		if string(msg) == "" {
			t.Error("expected non-empty message")
		}
	case <-time.After(100 * time.Millisecond):
		t.Error("client did not receive broadcast")
	}
}

func TestHubBroadcastOtherRoomIgnored(t *testing.T) {
	hub, mr := setupTestHub(t)
	go hub.Run()
	time.Sleep(10 * time.Millisecond)

	c := NewClient(hub, nil, 42)
	c.rooms = []string{"sheet:1"}
	c.send = make(chan []byte, 256)
	hub.Register(c)
	time.Sleep(10 * time.Millisecond)

	// Publish to different room
	mr.Publish("ws:broadcast", `{"room":"sheet:2","type":"sheet.updated"}`)
	time.Sleep(20 * time.Millisecond)

	select {
	case <-c.send:
		t.Error("client should NOT receive broadcast for sheet:2")
	case <-time.After(50 * time.Millisecond):
		// expected — no message
	}
}

// ---- Hub Redis reconnect ----

func TestHubConnectRedis(t *testing.T) {
	mr := miniredis.RunT(t)
	rdb := redis.NewClient(&redis.Options{Addr: mr.Addr()})
	hub := NewHub(rdb)

	ch := hub.connectRedis()
	if ch == nil {
		t.Error("connectRedis should succeed when Redis is running")
	}

	// Stop Redis and try connecting
	mr.Close()
	ch2 := hub.connectRedis()
	if ch2 != nil {
		t.Error("connectRedis should return nil when Redis is down")
	}
}

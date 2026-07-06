package ws

import (
	"encoding/json"
	"log/slog"
	"time"

	"github.com/redis/go-redis/v9"
)

// Hub maintains the set of active clients and broadcasts messages to rooms.
type Hub struct {
	rooms      map[string]map[*Client]bool
	register   chan *Client
	unregister chan *Client
	rdb        *redis.Client
}

// NewHub creates a new Hub with Redis pub/sub.
func NewHub(rdb *redis.Client) *Hub {
	return &Hub{
		rooms:      make(map[string]map[*Client]bool),
		register:   make(chan *Client),
		unregister: make(chan *Client),
		rdb:        rdb,
	}
}

// connectRedis tries to subscribe to the broadcast channel.
// Returns nil channel if Redis is not available.
func (h *Hub) connectRedis() <-chan *redis.Message {
	pubsub := h.rdb.Subscribe(nil, "ws:broadcast")
	_, err := pubsub.Receive(nil)
	if err != nil {
		pubsub.Close()
		return nil
	}
	return pubsub.Channel()
}

// Run starts the hub event loop. Retries Redis subscription indefinitely.
func (h *Hub) Run() {
	var redisCh <-chan *redis.Message
	retryTicker := time.NewTicker(5 * time.Second)
	defer retryTicker.Stop()

	// Initial connect
	redisCh = h.connectRedis()

	for {
		select {
		case client := <-h.register:
			for _, room := range client.rooms {
				if h.rooms[room] == nil {
					h.rooms[room] = make(map[*Client]bool)
				}
				h.rooms[room][client] = true
			}

		case client := <-h.unregister:
			for _, room := range client.rooms {
				if clients, ok := h.rooms[room]; ok {
					delete(clients, client)
					if len(clients) == 0 {
						delete(h.rooms, room)
					}
				}
			}
			close(client.send)

		case msg, ok := <-redisCh:
			if !ok {
				redisCh = nil
				continue
			}
			var broadcast struct {
				Room string `json:"room"`
			}
			if err := json.Unmarshal([]byte(msg.Payload), &broadcast); err != nil {
				continue
			}
			if clients, ok := h.rooms[broadcast.Room]; ok {
				for client := range clients {
					select {
					case client.send <- []byte(msg.Payload):
					default:
						go h.unregisterClient(client)
					}
				}
			}

		case <-retryTicker.C:
			if redisCh == nil {
				if ch := h.connectRedis(); ch != nil {
					redisCh = ch
					slog.Info("ws: Redis reconnected, broadcast enabled")
				}
			}
		}
	}
}

func (h *Hub) unregisterClient(c *Client) {
	h.unregister <- c
}

// Register adds a client to the hub.
func (h *Hub) Register(c *Client) {
	h.register <- c
	slog.Info("ws: client connected", "uid", c.uid, "rooms", c.rooms)
}

// Unregister removes a client from the hub.
func (h *Hub) Unregister(c *Client) {
	h.unregister <- c
	slog.Info("ws: client disconnected", "uid", c.uid)
}

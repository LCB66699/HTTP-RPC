package ws

import (
	"encoding/json"
	"log/slog"

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

// Run starts the hub event loop and Redis subscription.
func (h *Hub) Run() {
	pubsub := h.rdb.Subscribe(nil, "ws:broadcast")
	defer pubsub.Close()

	ch := pubsub.Channel()

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

		case msg := <-ch:
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

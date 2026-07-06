package ws

import (
	"encoding/json"
	"log/slog"
	"time"

	"github.com/gorilla/websocket"
)

const (
	writeWait      = 10 * time.Second
	pongWait       = 60 * time.Second
	pingPeriod     = (pongWait * 9) / 10
	maxMessageSize = 512
)

// Client represents a single WebSocket connection.
type Client struct {
	hub   *Hub
	conn  *websocket.Conn
	send  chan []byte
	uid   int64
	rooms []string
}

// wsMessage is the JSON protocol between client and server.
type wsMessage struct {
	Action string `json:"action"`            // subscribe | unsubscribe | ping
	Room   string `json:"room,omitempty"`    // "sheet:42"
	Type   string `json:"type,omitempty"`    // server → client message type
	Data   any    `json:"data,omitempty"`    // server → client payload
}

// NewClient creates a new WebSocket client.
func NewClient(hub *Hub, conn *websocket.Conn, uid int64) *Client {
	return &Client{
		hub:  hub,
		conn: conn,
		send: make(chan []byte, 256),
		uid:  uid,
	}
}

// readPump reads messages from the WebSocket connection.
func (c *Client) ReadPump() {
	defer func() {
		c.hub.Unregister(c)
		c.conn.Close()
	}()

	c.conn.SetReadLimit(maxMessageSize)
	c.conn.SetReadDeadline(time.Now().Add(pongWait))
	c.conn.SetPongHandler(func(string) error {
		c.conn.SetReadDeadline(time.Now().Add(pongWait))
		return nil
	})

	for {
		_, msgBytes, err := c.conn.ReadMessage()
		if err != nil {
			break
		}

		var msg wsMessage
		if err := json.Unmarshal(msgBytes, &msg); err != nil {
			continue
		}

		switch msg.Action {
		case "subscribe":
			if msg.Room != "" {
				c.rooms = append(c.rooms, msg.Room)
				c.hub.Register(c)
			}
		case "unsubscribe":
			if msg.Room != "" {
				for i, r := range c.rooms {
					if r == msg.Room {
						c.rooms = append(c.rooms[:i], c.rooms[i+1:]...)
						break
					}
				}
				c.hub.Unregister(c)
				if len(c.rooms) > 0 {
					c.hub.Register(c)
				}
			}
		case "ping":
			c.send <- []byte(`{"type":"pong"}`)
		}
	}
}

// WritePump writes messages to the WebSocket connection.
func (c *Client) WritePump() {
	ticker := time.NewTicker(pingPeriod)
	defer func() {
		ticker.Stop()
		c.conn.Close()
	}()

	for {
		select {
		case msg, ok := <-c.send:
			if !ok {
				c.conn.WriteMessage(websocket.CloseMessage, []byte{})
				return
			}
			c.conn.SetWriteDeadline(time.Now().Add(writeWait))
			if err := c.conn.WriteMessage(websocket.TextMessage, msg); err != nil {
				slog.Warn("ws: write error", "uid", c.uid, "error", err)
				return
			}

		case <-ticker.C:
			c.conn.SetWriteDeadline(time.Now().Add(writeWait))
			if err := c.conn.WriteMessage(websocket.PingMessage, nil); err != nil {
				return
			}
		}
	}
}

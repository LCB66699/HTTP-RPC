// Canal 兜底 — 从 Canal Server 订阅 binlog，补发 RabbitMQ 事件
package main

import (
	"encoding/json"
	"log"
	"time"

	"github.com/golang/protobuf/proto"
	amqp "github.com/rabbitmq/amqp091-go"
	"github.com/withlin/canal-go/client"
	pb "github.com/withlin/canal-go/protocol"
)

func startCanalFallback(ch *amqp.Channel) {
	addr := getenv("CANAL_ADDR", "canal-server")
	dest := getenv("CANAL_DEST", "spreadsheet")

	connector := client.NewSimpleCanalConnector(addr, 11111, "", "", dest, 60000, 60*60*1000)
	if err := connector.Connect(); err != nil {
		log.Printf("[Canal] connect failed: %v (fallback disabled)", err)
		return
	}
	if err := connector.Subscribe(".*\\..*"); err != nil {
		log.Printf("[Canal] subscribe failed: %v", err)
		return
	}
	log.Printf("[Canal] Connected to %s:11111, dest=%s", addr, dest)

	go func() {
		for {
			msg, err := connector.Get(100, nil, nil)
			if err != nil {
				log.Printf("[Canal] get error: %v, reconnecting...", err)
				connector.DisConnection()
				time.Sleep(5 * time.Second)
				if err := connector.Connect(); err != nil {
					continue
				}
				connector.Subscribe(".*\\..*")
				continue
			}

			for _, entry := range msg.Entries {
				et := entry.GetEntryType()
				if et != pb.EntryType_ROWDATA {
					continue
				}

				rc := &pb.RowChange{}
				if err := proto.Unmarshal(entry.GetStoreValue(), rc); err != nil {
					continue
				}
				if rc.GetIsDdl() {
					continue
				}

				header := entry.GetHeader()
				table := header.GetTableName()
				eventType := rc.GetEventType()

				var routingKey string
				switch {
				case table == "spreadsheets" && eventType == pb.EventType_INSERT:
					routingKey = "sheet.created"
				case table == "spreadsheets" && eventType == pb.EventType_UPDATE:
					routingKey = "sheet.updated"
				case table == "spreadsheets" && eventType == pb.EventType_DELETE:
					routingKey = "sheet.deleted"
				case table == "files" && eventType == pb.EventType_INSERT:
					routingKey = "file.uploaded"
				case table == "files" && eventType == pb.EventType_DELETE:
					routingKey = "file.deleted"
				default:
					continue
				}

				for _, rowData := range rc.GetRowDatas() {
					ev := map[string]interface{}{
						"type":             routingKey,
						"__canal_fallback": true,
						"timestamp":        time.Now().Unix(),
					}

					cols := rowData.GetAfterColumns()
					if eventType == pb.EventType_DELETE {
						cols = rowData.GetBeforeColumns()
					}
					for _, col := range cols {
						switch col.GetName() {
						case "id":
							v := col.GetValue()
							ev["id"] = v
							if table == "files" {
								ev["file_id"] = v
							}
						case "user_id":
							ev["user_id"] = col.GetValue()
						case "name":
							ev["name"] = col.GetValue()
						case "description":
							ev["description"] = col.GetValue()
						case "original_name":
							ev["original_name"] = col.GetValue()
						case "mime_type":
							ev["mime_type"] = col.GetValue()
						case "size":
							ev["size"] = col.GetValue()
						case "storage_path":
							ev["object_key"] = col.GetValue()
						}
					}

					body, _ := json.Marshal(ev)
					ch.Publish("rpc.events", routingKey, false, false,
						amqp.Publishing{ContentType: "application/json", Body: body})
					log.Printf("[Canal] fallback published: %s", routingKey)
				}
			}
		}
	}()
}

// Notify Service — RabbitMQ 事件消费者 + ES/MongoDB 索引
// 死信队列: 重试 5 分钟后进入 DLQ
// 订阅 file.* / sheet.* → MongoDB + Elasticsearch

package main

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"log"
	"os"
	"time"

	amqp "github.com/rabbitmq/amqp091-go"
	"github.com/minio/minio-go/v7"
	"github.com/minio/minio-go/v7/pkg/credentials"
	"go.mongodb.org/mongo-driver/mongo"
	"go.mongodb.org/mongo-driver/mongo/options"
	"github.com/elastic/go-elasticsearch/v8"
)

var (
	mongoColl  *mongo.Collection
	esClient   *elasticsearch.Client
	minioCli   *minio.Client
	minioBucket string
)

func main() {
	var err error
	esClient, _ = elasticsearch.NewClient(elasticsearch.Config{
		Addresses: []string{getenv("ES_HOST", "http://elasticsearch:9200")},
	})

	mongoClient, _ := mongo.Connect(context.Background(),
		options.Client().ApplyURI(getenv("MONGO_URI", "mongodb://mongodb:27017")))
	mongoColl = mongoClient.Database("rpc_search").Collection("doc_contents")

	// MinIO client
	minioEndpoint := getenv("MINIO_ENDPOINT", "minio:9000")
	minioBucket = getenv("MINIO_BUCKET", "rpc-files")
	minioCli, _ = minio.New(minioEndpoint, &minio.Options{
		Creds: credentials.NewStaticV4(
			getenv("MINIO_ACCESS_KEY", "rpc-minio"),
			getenv("MINIO_SECRET_KEY", "rpc-minio-123456"), ""),
		Secure: false,
	})
	log.Printf("[Notify] MinIO: %s/%s", minioEndpoint, minioBucket)

	rabbitHost := getenv("RABBITMQ_HOST", "rabbitmq")
	conn, err := amqp.Dial(fmt.Sprintf("amqp://%s:%s@%s:5672/",
		getenv("RABBITMQ_USER", "rpc"), getenv("RABBITMQ_PASS", "rpc-rabbit-123456"), rabbitHost))
	if err != nil { log.Fatalf("RabbitMQ: %v", err) }
	defer conn.Close()

	ch, _ := conn.Channel()
	defer ch.Close()
	ch.ExchangeDeclare("rpc.events", "topic", true, false, false, false, nil)

	// 死信交换机 + 死信队列
	ch.ExchangeDeclare("rpc.dlx", "topic", true, false, false, false, nil)
	dlq, _ := ch.QueueDeclare("notify.dlq", true, false, false, false, nil)
	ch.QueueBind(dlq.Name, "#", "rpc.dlx", false, nil)

	// 主队列: 绑定 DLX, 消息 5 分钟 TTL
	q, _ := ch.QueueDeclare("notify.doc.parse", true, false, false, false, amqp.Table{
		"x-dead-letter-exchange":    "rpc.dlx",
		"x-dead-letter-routing-key": "notify.failed",
		"x-message-ttl":             int32(300000),
	})
	ch.QueueBind(q.Name, "file.uploaded", "rpc.events", false, nil)
	ch.QueueBind(q.Name, "file.deleted", "rpc.events", false, nil)
	ch.QueueBind(q.Name, "sheet.created", "rpc.events", false, nil)
	ch.QueueBind(q.Name, "sheet.updated", "rpc.events", false, nil)
	ch.QueueBind(q.Name, "sheet.deleted", "rpc.events", false, nil)
	msgs, _ := ch.Consume(q.Name, "", false, false, false, false, nil)

	log.Println("[Notify] Listening for events (DLQ: notify.dlq)...")
	for msg := range msgs {
		var event map[string]interface{}
		if err := json.Unmarshal(msg.Body, &event); err != nil {
			msg.Nack(false, false)
			continue
		}
		var ok bool
		switch msg.RoutingKey {
		case "file.uploaded":
			ok = handleFileUploaded(event)
		case "file.deleted":
			ok = handleFileDeleted(event)
		case "sheet.created", "sheet.updated":
			ok = handleSheetUpsert(event)
		case "sheet.deleted":
			ok = handleSheetDelete(event)
		}
		if ok {
			msg.Ack(false)
		} else {
			msg.Nack(false, true)
		}
	}
}

func handleFileUploaded(event map[string]interface{}) bool {
	fileID := int64(event["file_id"].(float64))
	userID := int64(event["user_id"].(float64))
	origName, _ := event["original_name"].(string)
	mimeType, _ := event["mime_type"].(string)

	// Fetch content from MinIO if object_key present
	var contentText string
	var rawSize int64
	if objectKey, ok := event["object_key"].(string); ok && objectKey != "" && minioCli != nil {
		obj, err := minioCli.GetObject(context.Background(), minioBucket, objectKey, minio.GetObjectOptions{})
		if err == nil {
			defer obj.Close()
			buf := new(bytes.Buffer)
			written, _ := buf.ReadFrom(obj)
			rawSize = written
			contentText = buf.String()
			log.Printf("[Notify] MinIO content fetched: %s (%d bytes)", objectKey, rawSize)
		}
	}

	doc := map[string]interface{}{
		"file_id": fileID, "user_id": userID,
		"original_name": origName, "mime_type": mimeType,
		"parsed_at": time.Now().UTC(),
	}
	if contentText != "" {
		doc["content_text"] = contentText
		doc["raw_size"] = rawSize
	}
	if _, err := mongoColl.UpdateOne(context.Background(),
		map[string]interface{}{"file_id": fileID},
		map[string]interface{}{"$set": doc}, options.Update().SetUpsert(true)); err != nil {
		log.Printf("[Notify] MongoDB error: %v", err)
		return false
	}

	esDoc := map[string]interface{}{
		"id": fileID, "user_id": userID,
		"original_name": origName, "mime_type": mimeType,
		"type": "file",
	}
	body, _ := json.Marshal(esDoc)
	if _, err := esClient.Index("files_search", bytes.NewReader(body),
		esClient.Index.WithDocumentID(fmt.Sprintf("%d", fileID))); err != nil {
		log.Printf("[Notify] ES error: %v", err)
		return false
	}
	log.Printf("[Notify] Indexed file %d: %s", fileID, origName)
	return true
}

func handleFileDeleted(event map[string]interface{}) bool {
	fileID := int64(event["file_id"].(float64))
	if _, err := mongoColl.DeleteOne(context.Background(), map[string]interface{}{"file_id": fileID}); err != nil {
		return false
	}
	if _, err := esClient.Delete("files_search", fmt.Sprintf("%d", fileID)); err != nil {
		return false
	}
	log.Printf("[Notify] Deleted file %d", fileID)
	return true
}

func handleSheetUpsert(event map[string]interface{}) bool {
	sheetID := int64(event["id"].(float64))
	userID := int64(event["user_id"].(float64))
	name, _ := event["name"].(string)
	desc, _ := event["description"].(string)

	// Fetch content from MinIO if object_key present
	var cells, headers interface{}
	if objectKey, ok := event["object_key"].(string); ok && objectKey != "" && minioCli != nil {
		obj, err := minioCli.GetObject(context.Background(), minioBucket, objectKey, minio.GetObjectOptions{})
		if err == nil {
			defer obj.Close()
			var sheetData map[string]interface{}
			if json.NewDecoder(obj).Decode(&sheetData) == nil {
				cells = sheetData["data"]
				headers = sheetData["headers"]
				log.Printf("[Notify] MinIO sheet content fetched: %s", objectKey)
			}
		}
	}

	doc := map[string]interface{}{
		"sheet_id": sheetID, "user_id": userID,
		"name": name, "description": desc, "updated_at": time.Now().UTC(),
	}
	if cells != nil { doc["cells"] = cells }
	if headers != nil { doc["headers"] = headers }
	if _, err := mongoColl.Database().Collection("sheet_contents").UpdateOne(context.Background(),
		map[string]interface{}{"sheet_id": sheetID},
		map[string]interface{}{"$set": doc}, options.Update().SetUpsert(true)); err != nil {
		log.Printf("[Notify] MongoDB error: %v", err)
		return false
	}
	esDoc := map[string]interface{}{
		"id": sheetID, "user_id": userID,
		"name": name, "description": desc, "type": "sheet",
	}
	body, _ := json.Marshal(esDoc)
	if _, err := esClient.Index("sheets_search", bytes.NewReader(body),
		esClient.Index.WithDocumentID(fmt.Sprintf("%d", sheetID))); err != nil {
		log.Printf("[Notify] ES error: %v", err)
		return false
	}
	log.Printf("[Notify] Indexed sheet %d: %s", sheetID, name)
	return true
}

func handleSheetDelete(event map[string]interface{}) bool {
	sheetID := int64(event["id"].(float64))
	if _, err := mongoColl.Database().Collection("sheet_contents").DeleteOne(context.Background(),
		map[string]interface{}{"sheet_id": sheetID}); err != nil {
		return false
	}
	if _, err := esClient.Delete("sheets_search", fmt.Sprintf("%d", sheetID)); err != nil {
		return false
	}
	log.Printf("[Notify] Deleted sheet %d", sheetID)
	return true
}

func getenv(key, fallback string) string {
	if v := os.Getenv(key); v != "" { return v }
	return fallback
}

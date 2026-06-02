# ---- Configuration ----
CXX      := g++
CXXFLAGS := -std=c++20 -fcoroutines -Wall -O2 -I. -Iserver -Iserver/include -Iserver/generated -Igateway-cpp/include -I/usr/local/include
LDFLAGS_RPC := -lssl -lcrypto -lpthread -lnghttp2

PKG_CONFIG := $(shell command -v pkg-config 2>/dev/null)
ifneq ($(PKG_CONFIG),)
    LDFLAGS_RPC += $(shell pkg-config --libs grpc++ protobuf 2>/dev/null)
    MYSQL_LIBS := $(shell pkg-config --libs mysqlclient 2>/dev/null)
    HIREDIS_LIBS := $(shell pkg-config --libs hiredis 2>/dev/null)
    ifneq ($(MYSQL_LIBS),)
        LDFLAGS_RPC += $(MYSQL_LIBS)
    else
        LDFLAGS_RPC += -lmysqlclient
    endif
    ifneq ($(HIREDIS_LIBS),)
        LDFLAGS_RPC += $(HIREDIS_LIBS) -lredis++
    else
        LDFLAGS_RPC += -lhiredis -lredis++
    endif
else
    LDFLAGS_RPC += -lgrpc++ -lgrpc -lgrpc++_reflection -lgpr -lprotobuf -lmysqlclient -lhiredis -lredis++
endif

PROTO_DIR    := proto
SERVER_DIR   := server
GATEWAY_DIR  := gateway-cpp
GEN_CPP_DIR  := server/generated

PROTO_SRCS    := $(wildcard $(PROTO_DIR)/*.proto)
GEN_CC        := $(patsubst $(PROTO_DIR)/%.proto,$(GEN_CPP_DIR)/%.pb.cc,$(PROTO_SRCS))
GEN_GRPC_CC   := $(patsubst $(PROTO_DIR)/%.proto,$(GEN_CPP_DIR)/%.grpc.pb.cc,$(PROTO_SRCS))

# ---- gRPC Server (所有后端服务) ----
SERVER_SRCS := $(SERVER_DIR)/src/main.cpp \
               $(SERVER_DIR)/src/auth_service_impl.cpp \
               $(SERVER_DIR)/src/auth_interceptor.cpp \
               $(SERVER_DIR)/src/spreadsheet_service_impl.cpp \
               $(SERVER_DIR)/src/file_service_impl.cpp \
               $(SERVER_DIR)/src/health_service_impl.cpp \
               $(SERVER_DIR)/src/tx_resource.cpp \
               $(SERVER_DIR)/src/call_logger.cpp \
               $(SERVER_DIR)/src/database.cpp \
               $(SERVER_DIR)/src/redis_client.cpp \
               $(SERVER_DIR)/src/l1_cache.cpp \
               $(SERVER_DIR)/src/l1_invalidator.cpp

# ---- Gateway + TM ----
GATEWAY_SRCS := $(GATEWAY_DIR)/src/main.cpp \
                $(GATEWAY_DIR)/src/gateway.cpp \
                $(GATEWAY_DIR)/src/http2_server.cpp \
                $(SERVER_DIR)/src/tx_manager.cpp \
                $(SERVER_DIR)/src/database.cpp \
                $(SERVER_DIR)/src/redis_client.cpp

SERVER_TARGET := rpc_server
GATEWAY_TARGET := rpc_gateway

.PHONY: all proto server gateway clean run-server run-gateway

all: server gateway

proto:
	@echo "=== Generating Proto ==="
	@mkdir -p $(GEN_CPP_DIR)
	protoc -I $(PROTO_DIR) \
		--cpp_out=$(GEN_CPP_DIR) \
		--grpc_out=$(GEN_CPP_DIR) \
		--plugin=protoc-gen-grpc=$(shell which grpc_cpp_plugin) \
		$(PROTO_SRCS)
	@for f in $(GEN_CPP_DIR)/*.cc; do \
		sed -i 's|"proto/|"generated/|g' $$f 2>/dev/null || true; \
	done
	@echo "[OK] Proto generated"

server: proto
	@echo "=== Building gRPC Server ==="
	$(CXX) $(CXXFLAGS) -o $(SERVER_TARGET) $(SERVER_SRCS) $(GEN_CC) $(GEN_GRPC_CC) $(LDFLAGS_RPC)
	@echo "[OK] $(SERVER_TARGET)"

gateway: proto
	@echo "=== Building Gateway + TM ==="
	$(CXX) $(CXXFLAGS) -o $(GATEWAY_TARGET) $(GATEWAY_SRCS) $(GEN_CC) $(GEN_GRPC_CC) $(LDFLAGS_RPC)
	@echo "[OK] $(GATEWAY_TARGET)"

run-server:
	./$(SERVER_TARGET) --port 50051

run-gateway:
	./$(GATEWAY_TARGET) --port 8080 --grpc localhost:50051

clean:
	rm -f $(SERVER_TARGET) $(GATEWAY_TARGET)
	rm -f $(SERVER_DIR)/src/*.o $(GATEWAY_DIR)/src/*.o
	rm -rf $(GEN_CPP_DIR)

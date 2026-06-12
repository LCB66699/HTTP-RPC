# ---- Configuration ----
CXX      := g++
CXXFLAGS := -std=c++20 -fcoroutines -Wall -O2 -I. -Iserver -Iserver/include -Iserver/generated  -I/usr/local/include
LDFLAGS_RPC := -lssl -lcrypto -lpthread -lnghttp2 -lrabbitmq

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

# ---- gRPC Server (所有后端服�? ----
SERVER_SRCS := $(SERVER_DIR)/src/main.cpp \
               $(SERVER_DIR)/src/auth_service_impl.cpp \
               $(SERVER_DIR)/src/auth_interceptor.cpp \
               $(SERVER_DIR)/src/spreadsheet_service_impl.cpp \
               $(SERVER_DIR)/src/file_service_impl.cpp \
               $(SERVER_DIR)/src/health_service_impl.cpp \
               $(SERVER_DIR)/src/call_logger.cpp \
               $(SERVER_DIR)/src/database.cpp \
               $(SERVER_DIR)/src/redis_client.cpp \
               $(SERVER_DIR)/src/l1_cache.cpp \
               $(SERVER_DIR)/src/l1_invalidator.cpp \
               $(SERVER_DIR)/src/rabbit_publisher.cpp \
               $(SERVER_DIR)/src/search_service_impl.cpp

# ---- Gateway + TM ----
GATEWAY_SRCS := $(GATEWAY_DIR)/src/main.cpp \
                $(GATEWAY_DIR)/src/gateway.cpp \
                $(GATEWAY_DIR)/src/http2_server.cpp \
                $(SERVER_DIR)/src/database.cpp \
                $(SERVER_DIR)/src/redis_client.cpp

SERVER_TARGET := rpc_server
GATEWAY_TARGET := rpc_gateway

.PHONY: all proto server gateway clean run-server run-gateway auth sheet file search

all: server

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

# Shared source files (used by multiple services)
SHARED_SRCS := $(SERVER_DIR)/src/database.cpp \
               $(SERVER_DIR)/src/redis_client.cpp \
               $(SERVER_DIR)/src/call_logger.cpp \
               $(SERVER_DIR)/src/auth_interceptor.cpp

# Proto objects for each service
AUTH_PB   := $(GEN_CPP_DIR)/rpc_auth.pb.o $(GEN_CPP_DIR)/rpc_auth.grpc.pb.o $(GEN_CPP_DIR)/rpc_health.pb.o $(GEN_CPP_DIR)/rpc_health.grpc.pb.o
SHEET_PB  := $(GEN_CPP_DIR)/rpc_spreadsheet.pb.o $(GEN_CPP_DIR)/rpc_spreadsheet.grpc.pb.o $(GEN_CPP_DIR)/rpc_auth.pb.o $(GEN_CPP_DIR)/rpc_auth.grpc.pb.o $(GEN_CPP_DIR)/rpc_health.pb.o $(GEN_CPP_DIR)/rpc_health.grpc.pb.o
FILE_PB   := $(GEN_CPP_DIR)/rpc_file.pb.o $(GEN_CPP_DIR)/rpc_file.grpc.pb.o $(GEN_CPP_DIR)/rpc_auth.pb.o $(GEN_CPP_DIR)/rpc_auth.grpc.pb.o $(GEN_CPP_DIR)/rpc_health.pb.o $(GEN_CPP_DIR)/rpc_health.grpc.pb.o
SEARCH_PB := $(GEN_CPP_DIR)/rpc_search.pb.o $(GEN_CPP_DIR)/rpc_search.grpc.pb.o $(GEN_CPP_DIR)/rpc_health.pb.o $(GEN_CPP_DIR)/rpc_health.grpc.pb.o

auth: proto
	@echo "=== Building Auth Service ==="
	$(CXX) $(CXXFLAGS) -c -o $(GEN_CPP_DIR)/rpc_auth.pb.o $(GEN_CPP_DIR)/rpc_auth.pb.cc
	$(CXX) $(CXXFLAGS) -c -o $(GEN_CPP_DIR)/rpc_auth.grpc.pb.o $(GEN_CPP_DIR)/rpc_auth.grpc.pb.cc
	$(CXX) $(CXXFLAGS) -c -o $(GEN_CPP_DIR)/rpc_health.pb.o $(GEN_CPP_DIR)/rpc_health.pb.cc
	$(CXX) $(CXXFLAGS) -c -o $(GEN_CPP_DIR)/rpc_health.grpc.pb.o $(GEN_CPP_DIR)/rpc_health.grpc.pb.cc
	$(CXX) $(CXXFLAGS) -o rpc_auth \
		$(SERVER_DIR)/src/main_auth.cpp $(SERVER_DIR)/src/auth_service_impl.cpp $(SERVER_DIR)/src/health_service_impl.cpp $(SHARED_SRCS) $(AUTH_PB) $(LDFLAGS_RPC)
	@echo "[OK] rpc_auth"

sheet: proto
	@echo "=== Building Sheet Service ==="
	$(CXX) $(CXXFLAGS) -c -o $(GEN_CPP_DIR)/rpc_spreadsheet.pb.o $(GEN_CPP_DIR)/rpc_spreadsheet.pb.cc
	$(CXX) $(CXXFLAGS) -c -o $(GEN_CPP_DIR)/rpc_spreadsheet.grpc.pb.o $(GEN_CPP_DIR)/rpc_spreadsheet.grpc.pb.cc
	$(CXX) $(CXXFLAGS) -c -o $(GEN_CPP_DIR)/rpc_auth.pb.o $(GEN_CPP_DIR)/rpc_auth.pb.cc
	$(CXX) $(CXXFLAGS) -c -o $(GEN_CPP_DIR)/rpc_auth.grpc.pb.o $(GEN_CPP_DIR)/rpc_auth.grpc.pb.cc
	$(CXX) $(CXXFLAGS) -c -o $(GEN_CPP_DIR)/rpc_health.pb.o $(GEN_CPP_DIR)/rpc_health.pb.cc
	$(CXX) $(CXXFLAGS) -c -o $(GEN_CPP_DIR)/rpc_health.grpc.pb.o $(GEN_CPP_DIR)/rpc_health.grpc.pb.cc
	$(CXX) $(CXXFLAGS) -o rpc_sheet \
		$(SERVER_DIR)/src/main_sheet.cpp $(SERVER_DIR)/src/spreadsheet_service_impl.cpp $(SERVER_DIR)/src/health_service_impl.cpp $(SHARED_SRCS) \
		$(SERVER_DIR)/src/l1_cache.cpp $(SERVER_DIR)/src/l1_invalidator.cpp \
		$(SERVER_DIR)/src/rabbit_publisher.cpp
		$(SHEET_PB) $(LDFLAGS_RPC)
	@echo "[OK] rpc_sheet"

file: proto
	@echo "=== Building File Service ==="
	$(CXX) $(CXXFLAGS) -c -o $(GEN_CPP_DIR)/rpc_file.pb.o $(GEN_CPP_DIR)/rpc_file.pb.cc
	$(CXX) $(CXXFLAGS) -c -o $(GEN_CPP_DIR)/rpc_file.grpc.pb.o $(GEN_CPP_DIR)/rpc_file.grpc.pb.cc
	$(CXX) $(CXXFLAGS) -c -o $(GEN_CPP_DIR)/rpc_auth.pb.o $(GEN_CPP_DIR)/rpc_auth.pb.cc
	$(CXX) $(CXXFLAGS) -c -o $(GEN_CPP_DIR)/rpc_auth.grpc.pb.o $(GEN_CPP_DIR)/rpc_auth.grpc.pb.cc
	$(CXX) $(CXXFLAGS) -c -o $(GEN_CPP_DIR)/rpc_health.pb.o $(GEN_CPP_DIR)/rpc_health.pb.cc
	$(CXX) $(CXXFLAGS) -c -o $(GEN_CPP_DIR)/rpc_health.grpc.pb.o $(GEN_CPP_DIR)/rpc_health.grpc.pb.cc
	$(CXX) $(CXXFLAGS) -o rpc_file \
		$(SERVER_DIR)/src/main_file.cpp $(SERVER_DIR)/src/file_service_impl.cpp $(SERVER_DIR)/src/health_service_impl.cpp $(SHARED_SRCS) \
		$(SERVER_DIR)/src/l1_cache.cpp $(SERVER_DIR)/src/rabbit_publisher.cpp
		$(FILE_PB) $(LDFLAGS_RPC)
	@echo "[OK] rpc_file"

search: proto
	@echo "=== Building Search Service ==="
	$(CXX) $(CXXFLAGS) -c -o $(GEN_CPP_DIR)/rpc_search.pb.o $(GEN_CPP_DIR)/rpc_search.pb.cc
	$(CXX) $(CXXFLAGS) -c -o $(GEN_CPP_DIR)/rpc_search.grpc.pb.o $(GEN_CPP_DIR)/rpc_search.grpc.pb.cc
	$(CXX) $(CXXFLAGS) -c -o $(GEN_CPP_DIR)/rpc_health.pb.o $(GEN_CPP_DIR)/rpc_health.pb.cc
	$(CXX) $(CXXFLAGS) -c -o $(GEN_CPP_DIR)/rpc_health.grpc.pb.o $(GEN_CPP_DIR)/rpc_health.grpc.pb.cc
	$(CXX) $(CXXFLAGS) -o rpc_search \
		$(SERVER_DIR)/src/main_search.cpp $(SERVER_DIR)/src/search_service_impl.cpp $(SERVER_DIR)/src/health_service_impl.cpp $(SERVER_DIR)/src/redis_client.cpp $(SEARCH_PB) $(LDFLAGS_RPC)
	@echo "[OK] rpc_search"

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

# ---- Test Targets ----
.PHONY: test test-unit test-search test-grpc test-integration \
        test-functional test-performance test-stress test-all \
        test-docker-health test-smoke test-clean

TEST_DIR := test
LOGS_DIR := $(TEST_DIR)/logs

$(LOGS_DIR):
	@mkdir -p $(LOGS_DIR)

test-docker-health:
	@bash $(TEST_DIR)/docker_health.sh localhost

test-functional: $(LOGS_DIR) test-docker-health
	@echo "=== Functional Tests ==="
	@bash $(TEST_DIR)/functional_test.sh | tee $(LOGS_DIR)/functional.log

test-performance: test-docker-health
	@echo "=== Performance Tests ==="
	@bash $(TEST_DIR)/performance_test.sh | tee $(LOGS_DIR)/performance.log

test-stress: test-docker-health
	@echo "=== Stress Tests ==="
	@bash $(TEST_DIR)/stress_test.sh | tee $(LOGS_DIR)/stress.log

test-smoke: test-docker-health
	@echo "=== Smoke Tests ==="
	@bash $(TEST_DIR)/functional_test.sh | tail -5

test-all: test-docker-health
	@echo "=== Full Test Pipeline ==="
	@bash $(TEST_DIR)/functional_test.sh | tee $(LOGS_DIR)/functional.log
	@bash $(TEST_DIR)/performance_test.sh | tee $(LOGS_DIR)/performance.log
	@bash $(TEST_DIR)/stress_test.sh | tee $(LOGS_DIR)/stress.log
	@echo ""
	@echo "=========================================="
	@echo "  ALL TESTS COMPLETE"
	@echo "  Logs: $(LOGS_DIR)/"
	@echo "=========================================="

test-clean:
	rm -rf $(LOGS_DIR)
	rm -f /tmp/rpc_*.cookies /tmp/rpc_test_*
	@echo "Test logs cleaned"

test: test-smoke

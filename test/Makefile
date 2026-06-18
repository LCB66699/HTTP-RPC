# ---- Configuration ----
CXX      := g++
CXXFLAGS := -std=c++20 -Wall -O2 -I. -I../server -I../server/include -I../server/generated -I/usr/local/include
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

PROTO_DIR   := ../proto
SRC_DIR     := ../server/src
GEN_DIR     := ../server/generated

PROTO_SRCS   := $(wildcard $(PROTO_DIR)/*.proto)
GEN_CC       := $(patsubst $(PROTO_DIR)/%.proto,$(GEN_DIR)/%.pb.cc,$(PROTO_SRCS))
GEN_GRPC_CC  := $(patsubst $(PROTO_DIR)/%.proto,$(GEN_DIR)/%.grpc.pb.cc,$(PROTO_SRCS))

# ---- Shared sources ----
SHARED_SRCS := $(SRC_DIR)/shared/database.cpp \
               $(SRC_DIR)/shared/redis_client.cpp \
               $(SRC_DIR)/shared/call_logger.cpp \
               $(SRC_DIR)/shared/auth_interceptor.cpp

# ---- gRPC Services ----
.PHONY: all proto auth sheet file search clean

all: auth sheet file search

proto:
	@echo "=== Generating Proto ==="
	@mkdir -p $(GEN_DIR)
	protoc -I $(PROTO_DIR) \
		--cpp_out=$(GEN_DIR) \
		--grpc_out=$(GEN_DIR) \
		--plugin=protoc-gen-grpc=$(shell which grpc_cpp_plugin) \
		$(PROTO_SRCS)
	@for f in $(GEN_DIR)/*.cc; do \
		sed -i 's|"proto/|"generated/|g' $$f 2>/dev/null || true; \
	done
	@echo "[OK] Proto generated"

AUTH_PB := $(GEN_DIR)/rpc_auth.pb.o $(GEN_DIR)/rpc_auth.grpc.pb.o $(GEN_DIR)/rpc_health.pb.o $(GEN_DIR)/rpc_health.grpc.pb.o
SHEET_PB := $(GEN_DIR)/rpc_spreadsheet.pb.o $(GEN_DIR)/rpc_spreadsheet.grpc.pb.o $(GEN_DIR)/rpc_auth.pb.o $(GEN_DIR)/rpc_auth.grpc.pb.o $(GEN_DIR)/rpc_health.pb.o $(GEN_DIR)/rpc_health.grpc.pb.o
FILE_PB := $(GEN_DIR)/rpc_file.pb.o $(GEN_DIR)/rpc_file.grpc.pb.o $(GEN_DIR)/rpc_auth.pb.o $(GEN_DIR)/rpc_auth.grpc.pb.o $(GEN_DIR)/rpc_health.pb.o $(GEN_DIR)/rpc_health.grpc.pb.o
SEARCH_PB := $(GEN_DIR)/rpc_search.pb.o $(GEN_DIR)/rpc_search.grpc.pb.o

auth: proto
	@echo "=== Building Auth Service ==="
	$(CXX) $(CXXFLAGS) -c -o $(GEN_DIR)/rpc_auth.pb.o $(GEN_DIR)/rpc_auth.pb.cc
	$(CXX) $(CXXFLAGS) -c -o $(GEN_DIR)/rpc_auth.grpc.pb.o $(GEN_DIR)/rpc_auth.grpc.pb.cc
	$(CXX) $(CXXFLAGS) -c -o $(GEN_DIR)/rpc_health.pb.o $(GEN_DIR)/rpc_health.pb.cc
	$(CXX) $(CXXFLAGS) -c -o $(GEN_DIR)/rpc_health.grpc.pb.o $(GEN_DIR)/rpc_health.grpc.pb.cc
	$(CXX) $(CXXFLAGS) -o rpc_auth \
		$(SRC_DIR)/auth/main.cpp $(SRC_DIR)/auth/auth_service_impl.cpp \
		$(SRC_DIR)/shared/health_service_impl.cpp \
		$(SHARED_SRCS) $(AUTH_PB) $(LDFLAGS_RPC)
	@echo "[OK] rpc_auth"

sheet: proto
	@echo "=== Building Sheet Service ==="
	$(CXX) $(CXXFLAGS) -c -o $(GEN_DIR)/rpc_spreadsheet.pb.o $(GEN_DIR)/rpc_spreadsheet.pb.cc
	$(CXX) $(CXXFLAGS) -c -o $(GEN_DIR)/rpc_spreadsheet.grpc.pb.o $(GEN_DIR)/rpc_spreadsheet.grpc.pb.cc
	$(CXX) $(CXXFLAGS) -c -o $(GEN_DIR)/rpc_auth.pb.o $(GEN_DIR)/rpc_auth.pb.cc
	$(CXX) $(CXXFLAGS) -c -o $(GEN_DIR)/rpc_auth.grpc.pb.o $(GEN_DIR)/rpc_auth.grpc.pb.cc
	$(CXX) $(CXXFLAGS) -c -o $(GEN_DIR)/rpc_health.pb.o $(GEN_DIR)/rpc_health.pb.cc
	$(CXX) $(CXXFLAGS) -c -o $(GEN_DIR)/rpc_health.grpc.pb.o $(GEN_DIR)/rpc_health.grpc.pb.cc
	$(CXX) $(CXXFLAGS) -o rpc_sheet \
		$(SRC_DIR)/sheet/main.cpp $(SRC_DIR)/sheet/spreadsheet_service_impl.cpp \
		$(SRC_DIR)/shared/health_service_impl.cpp $(SRC_DIR)/shared/l1_cache.cpp \
		$(SRC_DIR)/shared/l1_invalidator.cpp $(SRC_DIR)/shared/rabbit_publisher.cpp \
		$(SHARED_SRCS) $(SHEET_PB) $(LDFLAGS_RPC)
	@echo "[OK] rpc_sheet"

file: proto
	@echo "=== Building File Service ==="
	$(CXX) $(CXXFLAGS) -c -o $(GEN_DIR)/rpc_file.pb.o $(GEN_DIR)/rpc_file.pb.cc
	$(CXX) $(CXXFLAGS) -c -o $(GEN_DIR)/rpc_file.grpc.pb.o $(GEN_DIR)/rpc_file.grpc.pb.cc
	$(CXX) $(CXXFLAGS) -c -o $(GEN_DIR)/rpc_auth.pb.o $(GEN_DIR)/rpc_auth.pb.cc
	$(CXX) $(CXXFLAGS) -c -o $(GEN_DIR)/rpc_auth.grpc.pb.o $(GEN_DIR)/rpc_auth.grpc.pb.cc
	$(CXX) $(CXXFLAGS) -c -o $(GEN_DIR)/rpc_health.pb.o $(GEN_DIR)/rpc_health.pb.cc
	$(CXX) $(CXXFLAGS) -c -o $(GEN_DIR)/rpc_health.grpc.pb.o $(GEN_DIR)/rpc_health.grpc.pb.cc
	$(CXX) $(CXXFLAGS) -o rpc_file \
		$(SRC_DIR)/file/main.cpp $(SRC_DIR)/file/file_service_impl.cpp \
		$(SRC_DIR)/shared/health_service_impl.cpp $(SRC_DIR)/shared/l1_cache.cpp \
		$(SRC_DIR)/shared/rabbit_publisher.cpp \
		$(SHARED_SRCS) $(FILE_PB) $(LDFLAGS_RPC)
	@echo "[OK] rpc_file"

search: proto
	@echo "=== Building Search Service ==="
	$(CXX) $(CXXFLAGS) -c -o $(GEN_DIR)/rpc_search.pb.o $(GEN_DIR)/rpc_search.pb.cc
	$(CXX) $(CXXFLAGS) -c -o $(GEN_DIR)/rpc_search.grpc.pb.o $(GEN_DIR)/rpc_search.grpc.pb.cc
	$(CXX) $(CXXFLAGS) -o rpc_search \
		$(SRC_DIR)/search/main.cpp $(SRC_DIR)/search/search_service_impl.cpp \
		$(SRC_DIR)/shared/health_service_impl.cpp \
		$(SEARCH_PB) $(LDFLAGS_RPC)
	@echo "[OK] rpc_search"

clean:
	rm -f rpc_auth rpc_sheet rpc_file rpc_search
	rm -rf $(GEN_DIR)

# ---- Test Targets ----
TEST_DIR := .
LOGS_DIR := $(TEST_DIR)/logs

$(LOGS_DIR):
	@mkdir -p $(LOGS_DIR)

.PHONY: test test-functional test-performance test-stress test-all test-docker-health test-smoke test-clean

test-docker-health:
	@bash $(TEST_DIR)/e2e/docker_health.sh localhost

test-functional: $(LOGS_DIR) test-docker-health
	@echo "=== Functional Tests ==="
	@bash $(TEST_DIR)/e2e/functional_test.sh | tee $(LOGS_DIR)/functional.log

test-performance: test-docker-health
	@echo "=== Performance Tests ==="
	@bash $(TEST_DIR)/e2e/performance_test.sh | tee $(LOGS_DIR)/performance.log

test-stress: test-docker-health
	@echo "=== Stress Tests ==="
	@bash $(TEST_DIR)/e2e/stress_test.sh | tee $(LOGS_DIR)/stress.log

test-smoke: test-docker-health
	@echo "=== Smoke Tests ==="
	@bash $(TEST_DIR)/e2e/functional_test.sh | tail -5

test-all: test-docker-health
	@echo "=== Full Test Pipeline ==="
	@bash $(TEST_DIR)/e2e/functional_test.sh | tee $(LOGS_DIR)/functional.log
	@bash $(TEST_DIR)/e2e/performance_test.sh | tee $(LOGS_DIR)/performance.log
	@bash $(TEST_DIR)/e2e/stress_test.sh | tee $(LOGS_DIR)/stress.log
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

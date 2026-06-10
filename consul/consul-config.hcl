# Consul 配置
data_dir = "/consul/data"
client_addr = "0.0.0.0"
bind_addr = "0.0.0.0"
server = true
bootstrap_expect = 1
ui_config { enabled = true }

ports {
  dns = 53
  http = 8500
  grpc = 8502
}

# 非 .consul 查询转发到 Docker DNS
recursors = ["127.0.0.11"]

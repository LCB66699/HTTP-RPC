# Consul 配置
data_dir = "/consul/data"
client_addr = "0.0.0.0"
bind_addr = "0.0.0.0"
server = true
bootstrap_expect = 1
ui_config { enabled = true }

ports {
  dns = 8600
  http = 8500
  grpc = 8502
}

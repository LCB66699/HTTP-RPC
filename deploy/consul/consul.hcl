data_dir = "/consul/data"
client_addr  = "0.0.0.0"
bind_addr    = "0.0.0.0"

ui_config {
  enabled = true
}

ports {
  grpc = 8502
}

server           = true
bootstrap_expect = 1

recursors = ["8.8.8.8", "1.1.1.1"]

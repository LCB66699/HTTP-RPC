FROM envoyproxy/envoy:v1.31-latest
COPY envoy/envoy.yaml /etc/envoy/envoy.yaml
CMD ["envoy", "-c", "/etc/envoy/envoy.yaml", "--log-level", "info"]

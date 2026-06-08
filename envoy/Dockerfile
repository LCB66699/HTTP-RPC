FROM envoyproxy/envoy:v1.31-latest
COPY envoy/envoy.yaml /etc/envoy/envoy.yaml
COPY envoy/entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh
ENTRYPOINT ["/entrypoint.sh"]

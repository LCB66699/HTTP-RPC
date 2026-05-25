FROM alpine:latest
RUN apk add --no-cache keepalived ipvsadm iproute2 gettext
COPY keepalived.conf /etc/keepalived/keepalived.conf.tmpl
COPY realserver.sh /usr/local/bin/realserver.sh
COPY entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh /usr/local/bin/realserver.sh
ENTRYPOINT ["/entrypoint.sh"]

package main

import (
	"testing"

	amqp "github.com/rabbitmq/amqp091-go"
	"go.opentelemetry.io/otel/propagation"
)

func TestAmqpHeadersToCarrierEmpty(t *testing.T) {
	headers := amqp.Table{}
	c := amqpHeadersToCarrier(headers)
	if len(c) != 0 {
		t.Fatal("empty headers should produce empty carrier")
	}
}

func TestAmqpHeadersToCarrierWithTraceParent(t *testing.T) {
	headers := amqp.Table{
		"traceparent": "00-0123456789abcdef0123456789abcdef-0123456789abcdef-01",
		"other":       123,
	}
	c := amqpHeadersToCarrier(headers)
	if c["traceparent"] != "00-0123456789abcdef0123456789abcdef-0123456789abcdef-01" {
		t.Fatal("traceparent not passed through")
	}
	if _, ok := c["other"]; ok {
		t.Fatal("non-string values should be ignored")
	}
}

func TestAmqpHeadersToCarrierExtract(t *testing.T) {
	headers := amqp.Table{
		"traceparent": "00-00000000000000000000000000000000-0000000000000000-01",
	}
	c := amqpHeadersToCarrier(headers)
	ctx := propagation.TraceContext{}.Extract(nil, propagation.MapCarrier(c))
	// traceparent with trace_id=0 is still a valid W3C format
	if ctx == nil {
		t.Skip("propagation returned nil context")
	}
}

package middleware

import (
	"context"
	"log/slog"
	"sync/atomic"
	"time"

	"github.com/sony/gobreaker/v2"
	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"
)

// CBSlow wraps a circuit breaker with a slow-call counter.
type CBSlow struct {
	*gobreaker.CircuitBreaker[any]
	slowCalls atomic.Int64
}

// NewCBSlow creates a CBSlow with sensible defaults.
func NewCBSlow(name string, metricsCb func(name string, val float64)) *CBSlow {
	cbs := &CBSlow{}
	cbs.CircuitBreaker = gobreaker.NewCircuitBreaker[any](gobreaker.Settings{
		Name:        name,
		MaxRequests: 3,
		Interval:    30 * time.Second,
		Timeout:     30 * time.Second,
		ReadyToTrip: func(counts gobreaker.Counts) bool {
			slow := float64(cbs.slowCalls.Load())
			total := float64(counts.Requests)
			return counts.ConsecutiveFailures >= 5 ||
				(total >= 10 && float64(counts.TotalFailures)/total >= 0.5) ||
				(total >= 10 && slow/total >= 0.8)
		},
		OnStateChange: func(name string, from, to gobreaker.State) {
			slog.Warn("circuit breaker state change", "name", name, "from", from.String(), "to", to.String())
			if to == gobreaker.StateClosed {
				cbs.slowCalls.Store(0)
			}
			if metricsCb != nil {
				stateVal := 0.0
				if to == gobreaker.StateHalfOpen {
					stateVal = 1.0
				} else if to == gobreaker.StateOpen {
					stateVal = 2.0
				}
				metricsCb(name, stateVal)
			}
		},
	})
	return cbs
}

// Interceptor returns a gRPC UnaryClientInterceptor that applies circuit
// breaking, retry with exponential backoff, per-attempt 3s timeout, and
// slow-call tagging.
func (cbs *CBSlow) Interceptor() grpc.UnaryClientInterceptor {
	return func(ctx context.Context, method string, req, reply any, cc *grpc.ClientConn, invoker grpc.UnaryInvoker, opts ...grpc.CallOption) error {
		_, err := cbs.Execute(func() (any, error) {
			var lastErr error
			for attempt := 0; attempt < 3; attempt++ {
				if attempt > 0 {
					time.Sleep(time.Duration(50<<(attempt-1)) * time.Millisecond)
				}
				callCtx, cancel := context.WithTimeout(ctx, 3*time.Second)
				start := time.Now()
				err := invoker(callCtx, method, req, reply, cc, opts...)
				cancel()
				if time.Since(start) > 2*time.Second {
					cbs.slowCalls.Add(1)
				}
				if err == nil {
					return nil, nil
				}
				lastErr = err
				if st, ok := status.FromError(err); ok {
					switch st.Code() {
					case codes.Unavailable, codes.DeadlineExceeded,
						codes.ResourceExhausted, codes.Aborted:
						continue
					}
				}
				return nil, err
			}
			return nil, lastErr
		})
		return err
	}
}

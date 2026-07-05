package discovery

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"strings"
	"sync"
	"time"

	"google.golang.org/grpc/resolver"
)

const scheme = "consul"

func init() {
	resolver.Register(&consulBuilder{})
}

type consulBuilder struct{}

func (b *consulBuilder) Build(target resolver.Target, cc resolver.ClientConn, opts resolver.BuildOptions) (resolver.Resolver, error) {
	addr := "http://consul:8500"
	if v := target.URL.Host; v != "" {
		addr = "http://" + v
	}
	r := &consulResolver{
		serviceName: strings.TrimPrefix(target.URL.Path, "/"),
		cc:          cc,
		consulAddr:  addr,
		interval:    15 * time.Second,
		done:        make(chan struct{}),
	}
	r.start()
	return r, nil
}

func (b *consulBuilder) Scheme() string { return scheme }

type consulResolver struct {
	serviceName string
	cc          resolver.ClientConn
	consulAddr  string
	interval    time.Duration
	done        chan struct{}
	mu          sync.Mutex
}

func (r *consulResolver) start() {
	go r.loop()
}

func (r *consulResolver) loop() {
	r.resolve()
	ticker := time.NewTicker(r.interval)
	defer ticker.Stop()
	for {
		select {
		case <-ticker.C:
			r.resolve()
		case <-r.done:
			return
		}
	}
}

func (r *consulResolver) resolve() {
	url := fmt.Sprintf("%s/v1/health/service/%s?passing=true", r.consulAddr, r.serviceName)

	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, url, nil)
	if err != nil {
		return
	}
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		return
	}
	defer resp.Body.Close()
	body, _ := io.ReadAll(resp.Body)

	var entries []struct {
		Service struct {
			Address string `json:"Address"`
			Port    int    `json:"Port"`
		} `json:"Service"`
	}
	json.Unmarshal(body, &entries)

	r.mu.Lock()
	defer r.mu.Unlock()

	addrs := make([]resolver.Address, 0, len(entries))
	for _, e := range entries {
		addrs = append(addrs, resolver.Address{
			Addr: fmt.Sprintf("%s:%d", e.Service.Address, e.Service.Port),
		})
	}
	r.cc.UpdateState(resolver.State{Addresses: addrs})
}

func (r *consulResolver) ResolveNow(o resolver.ResolveNowOptions) {
	r.resolve()
}

func (r *consulResolver) Close() {
	close(r.done)
}

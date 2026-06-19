-- wrk2 benchmark: pure gateway throughput baseline
-- Usage: wrk -t4 -c10 -d30s -R100 --latency -s health.lua https://localhost/api/v1/health
-- For wrk2 (constant-rate): wrk2 -t4 -c10 -d30s -R100 --latency -s health.lua https://localhost/api/v1/health

request = function()
    return wrk.format("GET", "/api/v1/health")
end

-- Optional: report custom latency histogram buckets
done = function(summary, latency, requests)
    io.write("------------------------------\n")
    io.write(string.format("Requests:  %d\n", summary.requests))
    io.write(string.format("Duration:  %.2fs\n", summary.duration / 1000000))
    io.write(string.format("QPS:       %.1f\n", summary.requests / (summary.duration / 1000000)))
    io.write(string.format("Errors:    %d (connect=%d, read=%d, write=%d, status=%d, timeout=%d)\n",
        summary.errors.connect + summary.errors.read + summary.errors.write + summary.errors.status + summary.errors.timeout,
        summary.errors.connect, summary.errors.read, summary.errors.write, summary.errors.status, summary.errors.timeout))
    io.write(string.format("Latency:   avg=%.2fms  p50=%.2fms  p90=%.2fms  p99=%.2fms  p99.9=%.2fms  max=%.2fms\n",
        latency.mean / 1000, latency:percentile(50) / 1000, latency:percentile(90) / 1000,
        latency:percentile(99) / 1000, latency:percentile(99.9) / 1000, latency.max / 1000))
end

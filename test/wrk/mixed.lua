-- wrk2 mixed read/write workload for HTTP-RPC
-- Simulates realistic traffic: 70% list, 20% get, 10% create
--
-- Prerequisites:
--   1. Set RPC_TOKEN env var or edit the token variable below
--   2. Create a test sheet first: the script auto-uses sheet ID 1 as default
--
-- Usage:
--   export RPC_TOKEN="eyJ..."   # get via: bash test/functional_test.sh  (check cookie jar)
--   wrk2 -t4 -c20 -d60s -R200 --latency -s mixed.lua https://localhost/api/v1/sheets
--
-- Customize load profile via env vars:
--   MIXED_LIST_PCT=70  MIXED_GET_PCT=20  MIXED_CREATE_PCT=10  (must sum to 100)
--   MIXED_SHEET_ID=1

local token = os.getenv("RPC_TOKEN") or ""
local sheet_id = tonumber(os.getenv("MIXED_SHEET_ID") or "1")

local list_pct = tonumber(os.getenv("MIXED_LIST_PCT") or "70")
local get_pct  = tonumber(os.getenv("MIXED_GET_PCT")  or "20")

-- thresholds
local list_threshold = list_pct
local get_threshold  = list_pct + get_pct

local counter = 0

request = function()
    counter = counter + 1
    local r = counter % 100  -- 0..99, distribution-driven
    local headers = {}
    headers["Content-Type"] = "application/json"
    if token ~= "" then
        headers["Cookie"] = "rpc_at=" .. token
    end

    if r < list_threshold then
        -- 70%: list spreadsheets (GET)
        return wrk.format("GET", "/api/v1/sheets?page=0&page_size=20", headers)
    elseif r < get_threshold then
        -- 20%: get single sheet (GET /api/v1/sheets/{id})
        return wrk.format("GET", "/api/v1/sheets/" .. sheet_id, headers)
    else
        -- 10%: create a new sheet (POST /api/v1/sheets)
        local body = string.format('{"name":"wrk-%d","headers_json":"[\\"A\\"]","data_json":"[[\\"x\\"]]"}', counter)
        return wrk.format("POST", "/api/v1/sheets", headers, body)
    end
end

done = function(summary, latency, requests)
    io.write("------------------------------\n")
    io.write(string.format("Profile:  %d%% list / %d%% get / %d%% create\n", list_pct, get_pct, 100 - list_pct - get_pct))
    io.write(string.format("Requests: %d (%.1f req/s)\n", summary.requests, summary.requests / (summary.duration / 1000000)))
    io.write(string.format("Errors:   connect=%d read=%d write=%d status=%d timeout=%d\n",
        summary.errors.connect, summary.errors.read, summary.errors.write,
        summary.errors.status, summary.errors.timeout))
    io.write(string.format("Latency:  avg=%.2fms  p50=%.2fms  p90=%.2fms  p99=%.2fms  p99.9=%.2fms  max=%.2fms\n",
        latency.mean / 1000, latency:percentile(50) / 1000, latency:percentile(90) / 1000,
        latency:percentile(99) / 1000, latency:percentile(99.9) / 1000, latency.max / 1000))
end

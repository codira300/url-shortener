param(
  [ValidateSet('redirect', 'mixed', 'rate-limit')]
  [string]$Mode = 'redirect',
  [int]$Rate = 1000,
  [string]$Duration = '20s',
  [string]$BaseUrl = 'http://127.0.0.1:8080',
  [int]$PreAllocatedVUs = 100,
  [int]$MaxVUs = 500,
  [int]$CreatePercent = 5,
  [string]$SeedCode = '',
  [switch]$SampleProcess,
  [string]$ProcessName = 'urlshortener',
  [switch]$ResetRateLimit
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$k6 = Join-Path $root 'tools\k6\k6-v0.54.0-windows-amd64\k6.exe'
if (-not (Test-Path $k6)) {
  $k6 = (Get-Command k6 -ErrorAction Stop).Source
}
$script = Join-Path $root "benchmarks\k6\$Mode.js"
$resultsDir = Join-Path $root 'benchmarks\results'
New-Item -ItemType Directory -Force -Path $resultsDir | Out-Null

try {
  $ready = Invoke-RestMethod -Uri "$BaseUrl/health/ready" -TimeoutSec 5
} catch {
  throw "service is not reachable at $BaseUrl`: $($_.Exception.Message)"
}
if ($ready.status -ne 'ready') {
  throw "service is not ready: $($ready | ConvertTo-Json -Compress)"
}

if ($ResetRateLimit) {
  $redisCli = Join-Path $root 'tools\redis\redis-cli.exe'
  if (Test-Path $redisCli) {
    $redisHost = if ($env:REDIS_HOST) { $env:REDIS_HOST } else { '127.0.0.1' }
    $redisPort = if ($env:REDIS_PORT) { $env:REDIS_PORT } else { '6380' }
    $keys = & $redisCli -h $redisHost -p $redisPort --scan --pattern 'rl:v2:*'
    foreach ($key in $keys) {
      if ($key) { & $redisCli -h $redisHost -p $redisPort DEL $key | Out-Null }
    }
  }
}

$code = $SeedCode
if (-not $code) {
  $body = @{ url = "https://example.com/bench/seed/$([DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds())"; expires_at = $null } | ConvertTo-Json
  $created = Invoke-RestMethod -Method Post -Uri "$BaseUrl/api/v1/urls" -ContentType 'application/json' -Body $body -TimeoutSec 5
  $code = $created.short_code
}

$env:BENCH_BASE_URL = $BaseUrl
$env:BENCH_CODE = $code
$env:BENCH_RATE = [string]$Rate
$env:BENCH_DURATION = $Duration
$env:BENCH_PRE_VUS = [string]$PreAllocatedVUs
$env:BENCH_MAX_VUS = [string]$MaxVUs
$env:BENCH_CREATE_PERCENT = [string]$CreatePercent

try {
  Invoke-WebRequest -Uri "$BaseUrl/$code" -MaximumRedirection 0 -SkipHttpErrorCheck -TimeoutSec 5 | Out-Null
} catch {
  # A 302 with MaximumRedirection 0 throws after the request has already warmed the cache.
}

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$summaryPath = Join-Path $resultsDir "$stamp-$Mode-$Rate.json"
$sampler = $null
$samples = @()
$k6Exit = 1

if ($SampleProcess) {
  $sampler = Start-Job -ScriptBlock {
    param($name)
    while ($true) {
      $procs = @(Get-Process -Name $name -ErrorAction SilentlyContinue)
      if ($procs.Count -gt 0) {
        $cpu = 0.0
        $workingSet = 0L
        foreach ($p in $procs) {
          $cpu += $p.TotalProcessorTime.TotalSeconds
          $workingSet += $p.WorkingSet64
        }
        [pscustomobject]@{ Timestamp = (Get-Date); CpuSeconds = $cpu; WorkingSet = $workingSet }
      }
      Start-Sleep -Milliseconds 500
    }
  } -ArgumentList $ProcessName
  Start-Sleep -Milliseconds 500
}

try {
  & $k6 run --summary-export $summaryPath --summary-time-unit ms --summary-trend-stats 'avg,min,med,max,p(50),p(90),p(95),p(99)' --no-color $script
  $k6Exit = $LASTEXITCODE
} finally {
  if ($sampler) {
    Stop-Job $sampler -ErrorAction SilentlyContinue
    $samples = @(Receive-Job $sampler -ErrorAction SilentlyContinue)
    Remove-Job $sampler -Force -ErrorAction SilentlyContinue
  }
}

if (-not (Test-Path $summaryPath)) {
  throw "k6 did not produce a summary (exit $k6Exit)"
}
$summary = Get-Content $summaryPath -Raw | ConvertFrom-Json
$metrics = $summary.metrics

function Get-MetricField($allMetrics, $metricName, $field, $defaultValue = 0) {
  $metric = $allMetrics.$metricName
  if ($null -eq $metric) { return $defaultValue }
  $source = $metric
  if ($metric.PSObject.Properties.Name -contains 'values') { $source = $metric.values }
  $property = $source.PSObject.Properties[$field]
  if ($null -eq $property) { return $defaultValue }
  return $property.Value
}

$achieved = Get-MetricField $metrics 'http_reqs' 'rate'
$count = Get-MetricField $metrics 'http_reqs' 'count'
$p50 = Get-MetricField $metrics 'http_req_duration' 'med' ''
$p90 = Get-MetricField $metrics 'http_req_duration' 'p(90)' ''
$p95 = Get-MetricField $metrics 'http_req_duration' 'p(95)' ''
$p99 = Get-MetricField $metrics 'http_req_duration' 'p(99)' ''
$max = Get-MetricField $metrics 'http_req_duration' 'max' ''
$failedRate = Get-MetricField $metrics 'http_req_failed' 'value'
if ($failedRate -eq 0) { $failedRate = Get-MetricField $metrics 'http_req_failed' 'rate' }
$dropped = Get-MetricField $metrics 'dropped_iterations' 'count'
$status302 = Get-MetricField $metrics 'status_302' 'count'
$status201 = Get-MetricField $metrics 'status_201' 'count'
$status429 = Get-MetricField $metrics 'status_429' 'count'
$statusOther = Get-MetricField $metrics 'status_other' 'count'

$avgCpu = ''
$maxCpu = ''
$avgWorkingSetMb = ''
$peakWorkingSetMb = ''
if ($samples.Count -ge 2) {
  $cores = [Environment]::ProcessorCount
  $first = $samples[0]
  $last = $samples[-1]
  $wall = ($last.Timestamp - $first.Timestamp).TotalSeconds
  if ($wall -gt 0) {
    $avgCpu = [math]::Round((($last.CpuSeconds - $first.CpuSeconds) / $wall / $cores) * 100, 1)
  }
  $peakCpu = 0.0
  $peakWorkingSet = 0L
  $workingSetTotal = 0L
  for ($i = 1; $i -lt $samples.Count; $i++) {
    $dt = ($samples[$i].Timestamp - $samples[$i - 1].Timestamp).TotalSeconds
    if ($dt -gt 0) {
      $pct = (($samples[$i].CpuSeconds - $samples[$i - 1].CpuSeconds) / $dt / $cores) * 100
      if ($pct -gt $peakCpu) { $peakCpu = $pct }
    }
    $workingSetTotal += $samples[$i].WorkingSet
    if ($samples[$i].WorkingSet -gt $peakWorkingSet) { $peakWorkingSet = $samples[$i].WorkingSet }
  }
  $maxCpu = [math]::Round($peakCpu, 1)
  $avgWorkingSetMb = [math]::Round(($workingSetTotal / ($samples.Count - 1)) / 1MB, 1)
  $peakWorkingSetMb = [math]::Round($peakWorkingSet / 1MB, 1)
}

$csvPath = Join-Path $resultsDir 'summary.csv'
if (-not (Test-Path $csvPath)) {
  'timestamp,mode,target_rps,duration,achieved_rps,requests,status_201,status_302,status_429,status_other,failed_rate,p50_ms,p90_ms,p95_ms,p99_ms,max_ms,dropped_iterations,avg_cpu_percent,max_cpu_percent,avg_working_set_mb,peak_working_set_mb,summary_file' |
    Set-Content -Path $csvPath
}
@(
  $stamp, $Mode, $Rate, $Duration, [math]::Round($achieved, 1), $count, $status201,
  $status302, $status429, $statusOther, [math]::Round($failedRate, 6), $p50, $p90, $p95, $p99,
  $max, $dropped, $avgCpu, $maxCpu, $avgWorkingSetMb, $peakWorkingSetMb, $summaryPath
) -join ',' | Add-Content -Path $csvPath

[pscustomobject]@{
  Mode = $Mode
  TargetRps = $Rate
  AchievedRps = [math]::Round($achieved, 1)
  Requests = $count
  Status201 = $status201
  Status302 = $status302
  Status429 = $status429
  StatusOther = $statusOther
  FailedRate = [math]::Round($failedRate, 6)
  P50ms = $p50
  P90ms = $p90
  P95ms = $p95
  P99ms = $p99
  MaxMs = $max
  DroppedIterations = $dropped
  AvgCpuPercent = $avgCpu
  MaxCpuPercent = $maxCpu
  AvgWorkingSetMb = $avgWorkingSetMb
  PeakWorkingSetMb = $peakWorkingSetMb
  SeedCode = $code
  SummaryFile = $summaryPath
} | Format-List

if ($k6Exit -ne 0) {
  throw "k6 exited with $k6Exit"
}

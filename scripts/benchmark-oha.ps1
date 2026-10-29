param(
  [int]$Rate = 10000,
  [int]$Connections = 100,
  [string]$Duration = '15s',
  [string]$BaseUrl = 'http://127.0.0.1:8080',
  [string]$SeedCode = '',
  [string]$OhaPath = '',
  [string]$OutputDir = ''
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent

if (-not $OhaPath) {
  $candidate = Join-Path $root 'tools\oha\oha.exe'
  if (Test-Path $candidate) {
    $OhaPath = $candidate
  } else {
    $command = Get-Command oha -ErrorAction SilentlyContinue
    if ($command) {
      $OhaPath = $command.Source
    }
  }
}
if (-not $OhaPath -or -not (Test-Path $OhaPath)) {
  throw 'oha was not found. Download it from https://github.com/hatoo/oha/releases and place it at tools\oha\oha.exe or pass -OhaPath.'
}

$resultsDir = if ($OutputDir) { $OutputDir } else { Join-Path $root 'benchmarks\results' }
New-Item -ItemType Directory -Force -Path $resultsDir | Out-Null

try {
  $ready = Invoke-RestMethod -Uri "$BaseUrl/health/ready" -TimeoutSec 5
} catch {
  throw "service is not reachable at $BaseUrl`: $($_.Exception.Message)"
}
if ($ready.status -ne 'ready') {
  throw "service is not ready: $($ready | ConvertTo-Json -Compress)"
}

$code = $SeedCode
if (-not $code) {
  $body = @{ url = "https://example.com/bench/oha/$([DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds())"; expires_at = $null } | ConvertTo-Json
  $created = Invoke-RestMethod -Method Post -Uri "$BaseUrl/api/v1/urls" -ContentType 'application/json' -Body $body -TimeoutSec 5
  $code = $created.short_code
}

try {
  Invoke-WebRequest -Uri "$BaseUrl/$code" -MaximumRedirection 0 -SkipHttpErrorCheck -TimeoutSec 5 | Out-Null
} catch {
  # A 302 with MaximumRedirection 0 throws after the request has already warmed the cache.
}

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$summaryPath = Join-Path $resultsDir "$stamp-oha-$Rate.json"
$logPath = Join-Path $resultsDir "$stamp-oha-$Rate.log"

$ohaArgs = @('--no-tui', '--output-format', 'json', '-o', $summaryPath, '-c', [string]$Connections, '-z', $Duration)
if ($Rate -gt 0) {
  $ohaArgs = @('-q', [string]$Rate) + $ohaArgs
}
$ohaArgs += "$BaseUrl/$code"

& $OhaPath @ohaArgs *> $logPath
$ohaExit = $LASTEXITCODE
if ((Test-Path $logPath) -and (Get-Item $logPath).Length -eq 0) {
  Remove-Item $logPath
}

if (-not (Test-Path $summaryPath)) {
  throw "oha did not produce a summary (exit $ohaExit). See $logPath"
}

$result = Get-Content $summaryPath -Raw | ConvertFrom-Json
$summary = $result.summary
$metrics = $result.metrics
$latency = $metrics.latency_ms

$statusCounts = @{}
$total = 0
foreach ($property in $result.statusCodeDistribution.PSObject.Properties) {
  $count = [int]$property.Value
  $statusCounts[$property.Name] = $count
  $total += $count
}

$status302 = if ($statusCounts.ContainsKey('302')) { $statusCounts['302'] } else { 0 }
$status2xx = 0
$status3xx = 0
$status429 = 0
foreach ($entry in $statusCounts.GetEnumerator()) {
  $parsed = 0
  if ([int]::TryParse($entry.Key, [ref]$parsed)) {
    if ($parsed -ge 200 -and $parsed -lt 300) { $status2xx += $entry.Value }
    if ($parsed -ge 300 -and $parsed -lt 400) { $status3xx += $entry.Value }
    if ($parsed -eq 429) { $status429 = $entry.Value }
  }
}
$successRate = if ($total -gt 0) { [math]::Round($status302 / $total, 6) } else { 0 }
$errorText = if ($result.errorDistribution) { $result.errorDistribution | ConvertTo-Json -Compress } else { '{}' }
$aborted = 0
if ($result.errorDistribution -and $result.errorDistribution.PSObject.Properties['aborted due to deadline']) {
  $aborted = [int]$result.errorDistribution.'aborted due to deadline'
}

$row = [pscustomobject]@{
  timestamp = $stamp
  target_rps = $Rate
  connections = $Connections
  duration = $Duration
  achieved_rps = [math]::Round($metrics.requests_per_sec, 1)
  total_requests = $total
  status_302 = $status302
  status_2xx = $status2xx
  status_3xx = $status3xx
  status_429 = $status429
  success_rate = $successRate
  p50_ms = $latency.p50
  p95_ms = $latency.p95
  p99_ms = $latency.p99
  max_ms = $latency.max
  aborted_deadline = $aborted
  errors = $errorText
  seed_code = $code
  summary_file = (Split-Path $summaryPath -Leaf)
}

$csvPath = Join-Path $resultsDir 'oha-summary.csv'
if (-not (Test-Path $csvPath)) {
  'timestamp,target_rps,connections,duration,achieved_rps,total_requests,status_302,status_2xx,status_3xx,status_429,success_rate,p50_ms,p95_ms,p99_ms,max_ms,aborted_deadline,errors,seed_code,summary_file' |
    Set-Content -Path $csvPath
}
@(
  $row.timestamp, $row.target_rps, $row.connections, $row.duration, $row.achieved_rps,
  $row.total_requests, $row.status_302, $row.status_2xx, $row.status_3xx, $row.status_429,
  $row.success_rate, $row.p50_ms, $row.p95_ms, $row.p99_ms, $row.max_ms,
  $row.aborted_deadline, $row.errors, $row.seed_code, $row.summary_file
) | ForEach-Object { '"' + ([string]$_ -replace '"', '""') + '"' } | Join-String -Separator ',' |
  Add-Content -Path $csvPath

$row | Format-List

if ($ohaExit -ne 0) {
  throw "oha exited with $ohaExit"
}

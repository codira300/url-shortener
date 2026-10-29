param(
  [ValidateSet('hot', 'cold', 'high-cardinality', 'missing')]
  [string]$Mode = 'hot',
  [int]$Codes = 200,
  [int]$Rate = 0,
  [int]$Connections = 100,
  [string]$Duration = '15s',
  [int]$Requests = 0,
  [string]$BaseUrl = 'http://127.0.0.1:8080',
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
    if ($command) { $OhaPath = $command.Source }
  }
}
if (-not $OhaPath -or -not (Test-Path $OhaPath)) {
  throw 'oha was not found. Place it at tools\oha\oha.exe or pass -OhaPath.'
}

$redisCli = Join-Path $root 'tools\redis\redis-cli.exe'
if ($Mode -eq 'cold' -and -not (Test-Path $redisCli)) {
  throw 'cold cache mode needs tools\redis\redis-cli.exe to delete seeded cache entries.'
}

if ($Mode -eq 'cold' -and $Connections -gt 10) {
  Write-Warning 'Cold-cache runs with duplicate in-flight misses can exceed the single-flight wait and return 503. Prefer -Connections 10 for a clean cold-start sample.'
}

$resultsDir = if ($OutputDir) { $OutputDir } else { Join-Path $root 'benchmarks\results' }
New-Item -ItemType Directory -Force -Path $resultsDir | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'

try {
  $ready = Invoke-RestMethod -Uri "$BaseUrl/health/ready" -TimeoutSec 5
} catch {
  throw "service is not reachable at $BaseUrl`: $($_.Exception.Message)"
}
if ($ready.status -ne 'ready') {
  throw "service is not ready: $($ready | ConvertTo-Json -Compress)"
}

function New-BenchCode([int]$index) {
  $body = @{
    url = "https://example.com/bench/workload/$stamp/$index"
    expires_at = $null
  } | ConvertTo-Json
  for ($attempt = 0; $attempt -lt 60; $attempt++) {
    try {
      $created = Invoke-RestMethod -Method Post -Uri "$BaseUrl/api/v1/urls" `
        -ContentType 'application/json' -Body $body -TimeoutSec 5
      return $created.short_code
    } catch {
      $status = $_.Exception.Response.StatusCode.value__
      if ($status -eq 429 -or $status -eq 503) {
        Start-Sleep -Milliseconds 250
        continue
      }
      throw
    }
  }
  throw "could not create benchmark code after retries: $index"
}

function Clear-SeededCache([string[]]$shortCodes) {
  $redisHost = if ($env:REDIS_HOST) { $env:REDIS_HOST } else { '127.0.0.1' }
  $redisPort = if ($env:REDIS_PORT) { $env:REDIS_PORT } else { '6380' }
  foreach ($code in $shortCodes) {
    & $redisCli -h $redisHost -p $redisPort DEL "url:v1:$code" | Out-Null
  }
}

function Invoke-Oha([string]$target, [string]$label, [int]$limitRequests) {
  $jsonPath = Join-Path $resultsDir "$stamp-workload-$Mode-$label.json"
  $logPath = Join-Path $resultsDir "$stamp-workload-$Mode-$label.log"
  $ohaArgs = @('--no-tui', '--output-format', 'json', '-o', $jsonPath, '-c', [string]$Connections)
  if ($Rate -gt 0) { $ohaArgs = @('-q', [string]$Rate) + $ohaArgs }
  if ($limitRequests -gt 0) {
    $ohaArgs += @('-n', [string]$limitRequests)
  } else {
    $ohaArgs += @('-z', $Duration)
  }
  if ($Mode -eq 'missing') { $ohaArgs += '--rand-regex-url' }
  if (Test-Path -LiteralPath $target -PathType Leaf) {
    $ohaArgs += @('--urls-from-file', $target)
  } else {
    $ohaArgs += $target
  }

  & $OhaPath @ohaArgs *> $logPath
  $exit = $LASTEXITCODE
  if ((Test-Path $logPath) -and (Get-Item $logPath).Length -eq 0) {
    Remove-Item $logPath
  }
  if (-not (Test-Path $jsonPath)) {
    throw "oha did not produce $jsonPath (exit $exit). See $logPath"
  }

  $result = Get-Content $jsonPath -Raw | ConvertFrom-Json
  $status = @{}
  $total = 0
  foreach ($property in $result.statusCodeDistribution.PSObject.Properties) {
    $status[$property.Name] = [int]$property.Value
    $total += [int]$property.Value
  }
  $status302 = if ($status.ContainsKey('302')) { $status['302'] } else { 0 }
  $status404 = if ($status.ContainsKey('404')) { $status['404'] } else { 0 }
  $status429 = if ($status.ContainsKey('429')) { $status['429'] } else { 0 }
  $statusOther = $total - $status302 - $status404 - $status429
  $aborted = 0
  if ($result.errorDistribution -and
      $result.errorDistribution.PSObject.Properties['aborted due to deadline']) {
    $aborted = [int]$result.errorDistribution.'aborted due to deadline'
  }

  [pscustomobject]@{
    timestamp = $stamp
    mode = $Mode
    label = $label
    codes = $Codes
    target_rps = $Rate
    connections = $Connections
    achieved_rps = [math]::Round($result.metrics.requests_per_sec, 1)
    total_requests = $total
    status_302 = $status302
    status_404 = $status404
    status_429 = $status429
    status_other = $statusOther
    aborted_deadline = $aborted
    p50_ms = $result.metrics.latency_ms.p50
    p95_ms = $result.metrics.latency_ms.p95
    p99_ms = $result.metrics.latency_ms.p99
    max_ms = $result.metrics.latency_ms.max
    summary_file = (Split-Path $jsonPath -Leaf)
  }
}

$rows = @()
switch ($Mode) {
  'missing' {
    $target = "$BaseUrl/[a-z][a-z][a-z][a-z][a-z][a-z][a-z][a-z]"
    $rows += Invoke-Oha $target 'missing' $Requests
  }
  'hot' {
    $code = New-BenchCode 0
    try {
      Invoke-WebRequest -Uri "$BaseUrl/$code" -MaximumRedirection 0 -SkipHttpErrorCheck -TimeoutSec 5 | Out-Null
    } catch {
      # A 302 with MaximumRedirection 0 throws after warming the cache.
    }
    $rows += Invoke-Oha "$BaseUrl/$code" 'hot' $Requests
  }
  default {
    $codesList = @()
    for ($i = 0; $i -lt $Codes; $i++) {
      $codesList += New-BenchCode $i
    }
    $urlFile = Join-Path $resultsDir "$stamp-workload-$Mode-urls.txt"
    $codesList | ForEach-Object { "$BaseUrl/$_" } | Set-Content -Path $urlFile

    if ($Mode -eq 'cold') {
      Clear-SeededCache $codesList
      $rows += Invoke-Oha $urlFile 'cold' $Codes
      # A second pass distinguishes first-hit/database cost from warm cache cost.
      $rows += Invoke-Oha $urlFile 'warm-after-cold' $Codes
    } else {
      Invoke-Oha $urlFile 'warmup' $Codes | Out-Null
      $rows += Invoke-Oha $urlFile 'high-cardinality' $Requests
    }
  }
}

$csvPath = Join-Path $resultsDir 'workloads-summary.csv'
if (-not (Test-Path $csvPath)) {
  'timestamp,mode,label,codes,target_rps,connections,achieved_rps,total_requests,status_302,status_404,status_429,status_other,aborted_deadline,p50_ms,p95_ms,p99_ms,max_ms,summary_file' |
    Set-Content -Path $csvPath
}
foreach ($row in $rows) {
  @(
    $row.timestamp, $row.mode, $row.label, $row.codes, $row.target_rps, $row.connections,
    $row.achieved_rps, $row.total_requests, $row.status_302, $row.status_404, $row.status_429,
    $row.status_other, $row.aborted_deadline, $row.p50_ms, $row.p95_ms, $row.p99_ms,
    $row.max_ms, $row.summary_file
  ) -join ',' | Add-Content -Path $csvPath
}
$rows | Format-Table -AutoSize

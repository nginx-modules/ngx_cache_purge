param(
    [Parameter(Mandatory = $true)][string] $Nginx,
    [string] $OutputDirectory = 't/test-results/windows'
)

$ErrorActionPreference = 'Stop'
if (-not $IsWindows) { throw 'This suite requires native Windows and PowerShell 7.' }
$Nginx = (Resolve-Path $Nginx).Path
New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
$OutputDirectory = (Resolve-Path $OutputDirectory).Path
$root = (New-Item -ItemType Directory (Join-Path $OutputDirectory ([guid]::NewGuid().ToString('N')))).FullName
$prefix = $root.Replace('\', '/') + '/'
$cache = $prefix + 'cache'
$process = $null
$failure = $null
$timer = [Diagnostics.Stopwatch]::StartNew()
Start-Transcript -Path (Join-Path $OutputDirectory 'runtime.log') | Out-Null

function Assert-True($Condition, [string] $Message) {
    if (-not $Condition) { throw $Message }
    Write-Host "PASS: $Message"
}

function Wait-Until([scriptblock] $Condition, [string] $Message) {
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    do {
        if (& $Condition) { return }
        Start-Sleep -Milliseconds 50
    } while ([DateTime]::UtcNow -lt $deadline)
    throw $Message
}

function Request([string] $Path, [string] $Method = 'GET', [hashtable] $Headers = @{}) {
    Invoke-WebRequest -Uri "http://127.0.0.1:$port$Path" -CustomMethod $Method `
        -Headers $Headers -SkipHttpErrorCheck -DisableKeepAlive -NoProxy -TimeoutSec 5
}

function Drain-Queue {
    Wait-Until {
        $response = Request '/status'
        if ($response.StatusCode -ne 200) { return $false }
        $state = $response.Content | ConvertFrom-Json
        $state.cache.pending.total -eq 0 -and $state.cache.active -eq 0
    } 'Queue did not drain'
}

function Nginx-Signal([string] $Signal) {
    & $Nginx -p $prefix -c conf/nginx.conf -s $Signal
    if ($LASTEXITCODE -ne 0) { throw "nginx -s $Signal failed: $LASTEXITCODE" }
}

try {
    foreach ($directory in @('conf', 'logs', 'origin', 'cache-real', 'cache-real/spool', 'outside')) {
        New-Item -ItemType Directory -Force (Join-Path $root $directory) | Out-Null
    }
    New-Item -ItemType Junction -Path (Join-Path $root 'cache') `
        -Target (Join-Path $root 'cache-real') | Out-Null
    [IO.File]::WriteAllText((Join-Path $root 'origin/item'), 'cached item')
    [IO.File]::WriteAllText((Join-Path $root 'origin/other'), 'another item')
    $marker = '0123456789abcdef0123456789abcdef'
    $protected = Join-Path $root "cache/spool/$marker"
    $outside = Join-Path $root "outside/$marker"
    [IO.File]::WriteAllText($protected, 'protected temporary file')
    [IO.File]::WriteAllText($outside, 'outside the cache')
    New-Item -ItemType Junction -Path (Join-Path $root 'cache/junction') `
        -Target (Join-Path $root 'outside') | Out-Null

    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
    $listener.Start()
    $port = $listener.LocalEndpoint.Port
    $listener.Stop()
    # Status resolution must normalize case, separators and trailing slashes.
    $statusPath = $cache.ToUpperInvariant().Replace('/', '\').Replace('\', '\\') + '///'
    $configuration = @'
worker_processes 1;
daemon off;
pid logs/nginx.pid;
error_log logs/error.log notice;
events { worker_connections 128; }
http {
    access_log off;
    cache_purge_namespace objects 1m "__ROOT__purge-state";
    client_body_temp_path "__ROOT__client-temp";
    proxy_temp_path "__ROOT__cache/spool";
    proxy_cache_path "__ROOT__cache" levels=1:2 keys_zone=a:2m inactive=1h;
    cache_purge_background_queue on;
    cache_purge_queue_size 32;
    cache_purge_batch_size 2;
    cache_purge_throttle_ms 10ms;
    cache_purge_response_type json;
    server {
        listen 127.0.0.1:__PORT__;
        root "__ROOT__";
        location /origin/ { }
        location /cache/ {
            proxy_pass http://127.0.0.1:__PORT__/origin/;
            proxy_cache a;
            proxy_cache_key $uri;
            proxy_cache_valid 200 1h;
            proxy_cache_purge PURGE from 127.0.0.1;
            add_header X-Cache $upstream_cache_status always;
        }
        location /all {
            proxy_cache a;
            proxy_cache_key $uri;
            proxy_cache_purge PURGE purge_all from 127.0.0.1;
        }
        location /strict/ {
            cache_purge_key objects $uri;
            proxy_pass http://127.0.0.1:__PORT__/origin/;
            proxy_set_header Host origin.test;
            proxy_cache a;
            proxy_cache_key $cache_purge_key;
            proxy_cache_valid 200 1h;
            proxy_cache_purge PURGE from 127.0.0.1;
            add_header X-Cache $upstream_cache_status always;
        }
        location = /refresh {
            cache_purge_key objects $http_x_refresh_key;
            proxy_cache_refresh a $cache_purge_key;
            cache_purge_refresh_profile /probe /strict;
            allow 127.0.0.1; deny all;
        }
        location = /probe {
            internal;
            proxy_cache off;
            proxy_pass_request_headers off;
            proxy_pass_request_body off;
            proxy_method HEAD;
            proxy_set_header Host origin.test;
            proxy_set_header Authorization "";
            proxy_set_header Cookie "";
            proxy_set_header If-None-Match $cache_purge_refresh_etag;
            proxy_set_header If-Modified-Since $cache_purge_refresh_last_modified;
            proxy_pass http://127.0.0.1:__PORT__/origin$cache_purge_refresh_uri;
        }
        location /denied {
            proxy_cache a;
            proxy_cache_key $uri;
            proxy_cache_purge PURGE from 192.0.2.1;
        }
        location /status {
            cache_purge_queue_status "__STATUS_PATH__";
            add_header X-Generation __GENERATION__ always;
        }
        location /response/json {
            proxy_cache a;
            proxy_cache_key $http_x_purge_key;
            proxy_cache_purge PURGE purge_all from 127.0.0.1;
        }
        location /response/xml {
            proxy_cache a;
            proxy_cache_key $http_x_purge_key;
            proxy_cache_purge PURGE purge_all from 127.0.0.1;
            cache_purge_response_type xml;
        }
    }
}
'@
    $configuration = $configuration.Replace('__ROOT__', $prefix).Replace('__PORT__', "$port").Replace('__STATUS_PATH__', $statusPath)
    $configPath = Join-Path $root 'conf/nginx.conf'
    [IO.File]::WriteAllText($configPath, $configuration.Replace('__GENERATION__', '1'))
    & $Nginx -p $prefix -c conf/nginx.conf -t
    if ($LASTEXITCODE -ne 0) { throw 'nginx configuration check failed' }
    $process = Start-Process -FilePath $Nginx -WorkingDirectory $root `
        -ArgumentList @('-p', ('"' + $prefix + '"'), '-c', 'conf/nginx.conf') -PassThru
    Wait-Until {
        try { (Request '/origin/item').StatusCode -eq 200 } catch { $false }
    } 'nginx did not become ready'

    Assert-True ((Request '/cache/item').Headers['X-Cache'] -eq 'MISS') 'Cache populates on MISS'
    Assert-True ((Request '/cache/item').Headers['X-Cache'] -eq 'HIT') 'Cache serves a HIT'
    # Copy a genuine native cache file outside the cache. An arbitrary marker
    # would be rejected by the header reader and could hide a junction bug.
    $nativeHash = [Convert]::ToHexString([Security.Cryptography.MD5]::HashData(
        [Text.Encoding]::UTF8.GetBytes('/cache/item'))).ToLowerInvariant()
    $nativeEntry = Get-ChildItem (Join-Path $root 'cache-real') -File -Recurse |
        Where-Object { $_.Name -eq $nativeHash } | Select-Object -First 1
    Assert-True ($null -ne $nativeEntry) 'Native cache entry is available for the junction regression'
    $outside = Join-Path $root ('outside/' + $nativeEntry.Name)
    [IO.File]::WriteAllBytes($outside, [IO.File]::ReadAllBytes($nativeEntry.FullName))
    $outsideDigest = (Get-FileHash $outside -Algorithm SHA256).Hash
    Assert-True ((Request '/cache/item' 'PURGE').StatusCode -eq 200) 'Exact purge completes'
    Assert-True ((Request '/cache/item').Headers['X-Cache'] -eq 'MISS') 'Exact purge invalidates cache metadata'
    Assert-True ((Request '/denied' 'PURGE').StatusCode -eq 403) 'Access list denies an unlisted client'
    Request '/cache/other' | Out-Null
    Assert-True ((Request '/cache/*' 'PURGE').StatusCode -eq 202) 'Wildcard purge is queued'
    Drain-Queue
    foreach ($item in @('item', 'other')) {
        Assert-True ((Request "/cache/$item").Headers['X-Cache'] -eq 'MISS') "Wildcard purge removes $item"
    }

    $key = 'A<&"\]]>Z'
    foreach ($format in @('json', 'xml')) {
        $response = Request "/response/$format" 'PURGE' @{'X-Purge-Key' = $key}
        Assert-True ($response.StatusCode -eq 202) "$format response is accepted"
        Assert-True ($response.Headers['Cache-Purge-Code'] -eq 'accepted') 'Result header is stable'
        $decoded = if ($format -eq 'json') {
            ($response.Content | ConvertFrom-Json).Key
        } else {
            ([xml] $response.Content).SelectSingleNode('//Key').InnerText
        }
        Assert-True ($decoded -ceq $key) "$format escaping preserves the key"
        Drain-Queue
    }
    Assert-True ((Request '/all' 'PURGE').StatusCode -eq 202) 'Full-cache purge is queued'
    Drain-Queue
    Assert-True (Test-Path $protected) 'Configured temporary path is protected'
    Assert-True (Test-Path $outside) 'Directory junction is not followed'
    Assert-True ((Get-FileHash $outside -Algorithm SHA256).Hash -eq $outsideDigest) `
        'Native cache file behind a descendant junction is unchanged'
    $response = Request '/status'
    $state = $response.Content | ConvertFrom-Json
    Assert-True ($response.Headers['Cache-Control'] -eq 'no-store') 'Status cannot be cached'
    Assert-True ($state.queue.max_size -eq 32 -and $state.queue.full -is [bool]) 'Status capacity and flag types are correct'
    Assert-True ($state.cache.files.deleted -ge 2) 'Background deletions are counted'
    $deleted = $state.cache.files.deleted

    Request '/strict/item' | Out-Null
    Assert-True ((Request '/strict/item').Headers['X-Cache'] -eq 'HIT') 'Generation key serves HIT'
    $result = (Request '/refresh' 'REFRESH' @{'X-Refresh-Key' = '/strict/item'}).Content | ConvertFrom-Json
    Assert-True ($result.complete -and $result.results[0].result -eq 'kept') 'Conditional HEAD retains a current generation'
    Assert-True ((Request '/strict/item' 'PURGE').StatusCode -eq 200) 'Logical invalidation completes on Windows'
    Assert-True ((Request '/strict/item').Headers['X-Cache'] -eq 'MISS') 'Next GET uses a new generation'
    Drain-Queue
    $state = (Request '/status').Content | ConvertFrom-Json
    $deleted = $state.cache.files.deleted
    Assert-True ($state.cache.logical.invalidations -eq 1) 'Logical invalidations have a separate counter'

    [IO.File]::WriteAllText($configPath, $configuration.Replace('__GENERATION__', '2'))
    Nginx-Signal 'reload'
    Wait-Until { (Request '/status').Headers['X-Generation'] -eq '2' } 'Reload did not activate the new configuration'
    $state = (Request '/status').Content | ConvertFrom-Json
    Assert-True ($state.cache.files.deleted -eq $deleted) 'Counters survive reload'
    Assert-True ((Request '/strict/item').Headers['X-Cache'] -eq 'HIT') 'Reload preserves generation state'
    Request '/cache/item' | Out-Null
    Assert-True ((Request '/cache/*' 'PURGE').StatusCode -eq 202) 'Queue accepts work after reload'
    Drain-Queue
    Nginx-Signal 'quit'
    Assert-True ($process.WaitForExit(15000)) 'Graceful shutdown completes'
    $log = [IO.File]::ReadAllText((Join-Path $root 'logs/error.log'))
    Assert-True ($log -notmatch '\[(crit|alert|emerg)\]') 'No unexpected severe diagnostics'
} catch {
    $failure = $_
    Write-Host ($_ | Out-String)
} finally {
    if ($process -and -not $process.HasExited) {
        try { Nginx-Signal 'stop' } catch { Write-Warning $_ }
        if (-not $process.WaitForExit(5000)) {
            & taskkill.exe /PID $process.Id /T /F | Out-Null
        }
    }
    $timer.Stop()
    $xml = [Xml.XmlWriter]::Create((Join-Path $OutputDirectory 'windows.xml'))
    $xml.WriteStartElement('testsuites')
    $xml.WriteStartElement('testsuite')
    $xml.WriteAttributeString('name', 'windows_runtime')
    $xml.WriteAttributeString('tests', '1')
    $xml.WriteAttributeString('failures', $(if ($failure) { '1' } else { '0' }))
    $xml.WriteStartElement('testcase')
    $xml.WriteAttributeString('name', 'native purge lifecycle')
    $xml.WriteAttributeString('time', $timer.Elapsed.TotalSeconds.ToString([Globalization.CultureInfo]::InvariantCulture))
    if ($failure) {
        $xml.WriteStartElement('failure')
        $xml.WriteAttributeString('message', $failure.Exception.Message)
        $xml.WriteString(($failure | Out-String))
        $xml.WriteEndElement()
    }
    $xml.WriteEndElement()
    $xml.WriteEndElement()
    $xml.WriteEndElement()
    $xml.Dispose()
    Stop-Transcript | Out-Null
}
if ($failure) { exit 1 }

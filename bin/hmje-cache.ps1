param(
    [Parameter(Mandatory = $true)][string]$Root,
    [switch]$Automatic,
    [long]$MaxGiB = -1,
    [long]$MinAgeMinutes = -1
)

$ErrorActionPreference = 'Stop'
# Same policy as tools/cache-prune.c. Malformed environment values fall back
# to the defaults so automatic maintenance never fails a build.
function PolicyValue([string]$name, [long]$default, [long]$low, [long]$high) {
    $raw = [Environment]::GetEnvironmentVariable($name)
    [long]$parsed = 0
    if ($raw -and [long]::TryParse($raw, [ref]$parsed) -and $parsed -ge $low -and $parsed -le $high) { return $parsed }
    return $default
}
if ($MaxGiB -lt 0) { $MaxGiB = PolicyValue 'HMJE_CACHE_MAX_GIB' 10 1 1024 }
if ($MinAgeMinutes -lt 0) { $MinAgeMinutes = PolicyValue 'HMJE_CACHE_MIN_AGE_MINUTES' 60 0 5256000 }
$intervalMinutes = PolicyValue 'HMJE_CACHE_GC_INTERVAL_MINUTES' 60 0 5256000
$lowFreeGiB = PolicyValue 'HMJE_CACHE_LOW_FREE_GIB' 15 0 1048576
$lowFreePercent = PolicyValue 'HMJE_CACHE_LOW_FREE_PERCENT' 5 0 100
$lowMaxGiB = PolicyValue 'HMJE_CACHE_LOW_MAX_GIB' 4 1 1024
$lowMinAgeMinutes = PolicyValue 'HMJE_CACHE_LOW_MIN_AGE_MINUTES' 10 0 5256000
$lowIntervalMinutes = [Math]::Min((PolicyValue 'HMJE_CACHE_LOW_INTERVAL_MINUTES' 5 0 5256000), $intervalMinutes)
if ($MaxGiB -lt 1 -or $MaxGiB -gt 1024 -or $MinAgeMinutes -gt 5256000) {
    throw 'cache limits are out of range'
}
$rootPath = [IO.Path]::GetFullPath($Root)
if (!(Test-Path -LiteralPath $rootPath -PathType Container)) { exit 0 }
$now = [DateTime]::UtcNow
$stamp = Join-Path $rootPath '.gc-last'
$sinceLast = $null
if (Test-Path -LiteralPath $stamp) { $sinceLast = ($now - [IO.File]::GetLastWriteTimeUtc($stamp)).TotalMinutes }
if ($Automatic -and $null -ne $sinceLast -and $sinceLast -ge 0 -and $sinceLast -lt $lowIntervalMinutes) { exit 0 }
$lowSpace = $false
try {
    $drive = [IO.DriveInfo]::new([IO.Path]::GetPathRoot($rootPath))
    [long]$free = $drive.AvailableFreeSpace
    [long]$forced = 0
    if ($env:HMJE_CACHE_FREE_BYTES -and [long]::TryParse($env:HMJE_CACHE_FREE_BYTES, [ref]$forced)) { $free = $forced }
    $threshold = [Math]::Max($lowFreeGiB * 1GB, [long]($drive.TotalSize / 100 * $lowFreePercent))
    $lowSpace = $free -lt $threshold
} catch { $lowSpace = $false }
if ($Automatic -and !$lowSpace -and $null -ne $sinceLast -and $sinceLast -ge 0 -and $sinceLast -lt $intervalMinutes) { exit 0 }
if ($lowSpace) {
    $MaxGiB = [Math]::Min($MaxGiB, $lowMaxGiB)
    $MinAgeMinutes = [Math]::Min($MinAgeMinutes, $lowMinAgeMinutes)
}
$lockPath = Join-Path $rootPath '.gc-lock'
try {
    $guard = [IO.File]::Open($lockPath, [IO.FileMode]::OpenOrCreate,
                             [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
} catch [IO.IOException] { exit 0 }
try {
    $entries = New-Object 'System.Collections.Generic.List[object]'
    $pending = New-Object 'System.Collections.Generic.Stack[string]'
    foreach ($part in @('parse', 'products')) {
        $path = Join-Path $rootPath $part
        if (Test-Path -LiteralPath $path -PathType Container) { $pending.Push($path) }
    }
    [long]$total = 0
    while ($pending.Count -gt 0) {
        $directory = $pending.Pop()
        foreach ($path in [IO.Directory]::EnumerateFileSystemEntries($directory)) {
            $name = [IO.Path]::GetFileName($path)
            $attributes = [IO.File]::GetAttributes($path)
            if (($attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { continue }
            if (($attributes -band [IO.FileAttributes]::Directory) -ne 0) {
                if ($name.StartsWith('work-') -or $name.StartsWith('invocation.') -or
                    $name.StartsWith('.tmp') -or $name.Contains('.tmp.')) { continue }
                $pending.Push($path)
                continue
            }
            if ($name.StartsWith('.tmp') -or $name.Contains('.tmp.') -or
                $name -eq '.publish.lock') { continue }
            $info = [IO.FileInfo]::new($path)
            $used = $info.LastWriteTimeUtc
            if ($info.LastAccessTimeUtc -gt $used) { $used = $info.LastAccessTimeUtc }
            $total += $info.Length
            if (($now - $used).TotalMinutes -ge $MinAgeMinutes) {
                $entries.Add([pscustomobject]@{ Path=$path; Length=$info.Length; Used=$used })
            }
        }
    }
    $before = $total
    [long]$removed = 0
    $limit = $MaxGiB * 1GB
    if ($total -gt $limit) {
        foreach ($item in ($entries | Sort-Object Used)) {
            if ($total -le $limit) { break }
            try {
                $info = [IO.FileInfo]::new($item.Path)
                if (!$info.Exists -or $info.Length -ne $item.Length -or
                    (($info.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)) { continue }
                $used = $info.LastWriteTimeUtc
                if ($info.LastAccessTimeUtc -gt $used) { $used = $info.LastAccessTimeUtc }
                if ($used -ne $item.Used -or ($now - $used).TotalMinutes -lt $MinAgeMinutes) { continue }
                [IO.File]::Delete($item.Path)
                $total -= $item.Length
                $removed++
            } catch [IO.IOException] { continue }
              catch [UnauthorizedAccessException] { continue }
        }
    }
    [IO.File]::WriteAllText($stamp, '')
    if (!$Automatic) { Write-Output "hmjec-cache: before=$before after=$total removed=$removed mode=$(if ($lowSpace) { 'low-space' } else { 'normal' }) budget=$limit min-age-minutes=$MinAgeMinutes" }
} finally {
    $guard.Dispose()
}

param(
    [Parameter(Mandatory = $true)][string]$OutputDirectory
)

# Windows counterpart of tools/vendor-platform-libraries.sh: export the commits pinned in
# platform-libraries.lock into OutputDirectory/platform-libraries with `git archive`.
# Lookup order per repository: HMJE_PLATFORM_SOURCES (semicolon separated directories),
# the toolchain checkout's siblings, then a bare mirror under the hmje cache that is
# fetched from the lock URL unless HMJE_PLATFORM_OFFLINE=1.
# HMJE_PLATFORM_PROFILE=public vendors only platform-libraries.lock; internal (the default in this internal repository) adds
# platform-libraries.internal.lock (docs/platform-libraries.md §1.1).
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$lock = if ($env:HMJE_PLATFORM_LOCK) { $env:HMJE_PLATFORM_LOCK } else { Join-Path $root 'platform-libraries.lock' }
if (!(Test-Path -LiteralPath $lock -PathType Leaf)) { throw "missing platform library lock: $lock" }
$internalLock = if ($env:HMJE_PLATFORM_INTERNAL_LOCK) { $env:HMJE_PLATFORM_INTERNAL_LOCK } else { Join-Path $root 'platform-libraries.internal.lock' }
$defaults = @{}
Get-Content -LiteralPath (Join-Path $PSScriptRoot 'platform-libraries.defaults') -Encoding UTF8 | ForEach-Object {
    if ($_ -match '^(profile|base_url)=') {
        $key, $value = $_ -split '=', 2
        $defaults[$key] = $value.Trim()
    }
}
$profile = if ($env:HMJE_PLATFORM_PROFILE) { $env:HMJE_PLATFORM_PROFILE } else { $defaults['profile'] }
if ($profile -ne 'public' -and $profile -ne 'internal') { throw "HMJE_PLATFORM_PROFILE must be public or internal, not $profile" }
function LockNames([string]$Path) {
    Get-Content -LiteralPath $Path -Encoding UTF8 | ForEach-Object { $_.Trim() } |
        Where-Object { $_ -and !$_.StartsWith('#') } | ForEach-Object { ($_ -split '\s+')[0] }
}
$lockLines = @(Get-Content -LiteralPath $lock -Encoding UTF8)
if ($profile -eq 'internal') {
    if (!(Test-Path -LiteralPath $internalLock -PathType Leaf)) { throw "internal profile requires $internalLock" }
    $publicNames = @(LockNames $lock)
    foreach ($internal in (LockNames $internalLock)) {
        if ($publicNames -contains $internal) { throw "internal platform library $internal is also in the public lock $lock" }
    }
    if ($profile -eq 'internal') { $lockLines += @(Get-Content -LiteralPath $internalLock -Encoding UTF8) }
}
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$output = Join-Path ([IO.Path]::GetFullPath($OutputDirectory)) 'platform-libraries'
$monorepo = Join-Path $root 'libraries'
if ($profile -eq 'public' -and (Test-Path -LiteralPath $monorepo -PathType Container)) {
    $expectedIndex = @($lockLines | Where-Object { $_.Trim() -and !$_.Trim().StartsWith('#') } | ForEach-Object {
        $fields = $_.Trim() -split '\s+'
        if ($fields.Count -ne 4) { throw "malformed lock line: $_" }
        "$($fields[0])`t$($fields[2])`t$($fields[1])`t$($fields[3])"
    })
    $actualIndex = @(Get-Content -LiteralPath (Join-Path $monorepo 'index.tsv') -Encoding UTF8 | Where-Object { $_ -and !$_.StartsWith('#') })
    if (($expectedIndex -join "`n") -cne ($actualIndex -join "`n")) {
        throw 'monorepo library index does not match the selected public lock'
    }
}
$cacheBase = if ($env:HMJE_CACHE_HOME) { $env:HMJE_CACHE_HOME } elseif ($env:LOCALAPPDATA) { Join-Path $env:LOCALAPPDATA 'hmje' } else { throw 'LOCALAPPDATA or HMJE_CACHE_HOME is required' }
$mirrors = Join-Path $cacheBase 'platform-sources'
$staging = Join-Path ([IO.Path]::GetTempPath()) ('hmje-platform-' + [guid]::NewGuid().ToString('N'))
$out = Join-Path $staging 'out'
New-Item -ItemType Directory -Force -Path $out | Out-Null

function HasCommit([string]$Directory, [string]$Commit) {
    if (!(Test-Path -LiteralPath $Directory)) { return $false }
    # A candidate can be an exported source tree rather than a Git checkout.
    # PowerShell 5.1 promotes redirected native stderr to an error record; a
    # failed probe must let FindSource try the next directory or cached mirror.
    $savedPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & git -C $Directory rev-parse -q --verify "$Commit^{commit}" *> $null
        return $LASTEXITCODE -eq 0
    } finally {
        $ErrorActionPreference = $savedPreference
    }
}

function FindSource([string]$Repo, [string]$Commit, [string]$Url) {
    if ($Repo -eq "hmje-toolchain" -and (HasCommit $root $Commit)) { return $root }
    $directories = @()
    if ($env:HMJE_PLATFORM_SOURCES) { $directories += $env:HMJE_PLATFORM_SOURCES.Split(';') | Where-Object { $_ } }
    $directories += (Join-Path $root '..')
    foreach ($directory in $directories) {
        foreach ($candidate in @((Join-Path $directory $Repo), (Join-Path $directory "$Repo.git"))) {
            if (HasCommit $candidate $Commit) { return $candidate }
        }
    }
    $mirror = Join-Path $mirrors "$Repo.git"
    if (HasCommit $mirror $Commit) { return $mirror }
    if ($env:HMJE_PLATFORM_OFFLINE -eq '1') { throw "$Repo $Commit is not available offline (set HMJE_PLATFORM_SOURCES)" }
    if ($Url -notmatch '://|^git@') {
        $base = if ($env:HMJE_PLATFORM_BASE_URL) { $env:HMJE_PLATFORM_BASE_URL } else { $defaults['base_url'] }
        if (!$base) { throw 'set HMJE_PLATFORM_BASE_URL to the library repository base URL, or provide pinned local sources with HMJE_PLATFORM_SOURCES (public hosting is not configured)' }
        $Url = $base.TrimEnd('/') + '/' + $Url
    }
    New-Item -ItemType Directory -Force -Path $mirrors | Out-Null
    if (!(Test-Path -LiteralPath $mirror)) {
        & git init -q --bare $mirror
        if ($LASTEXITCODE -ne 0) { throw "git init failed: $mirror" }
    }
    [Console]::Error.WriteLine("fetching $Url for $Repo $Commit")
    & git -C $mirror fetch -q $Url '+refs/heads/*:refs/heads/*' '+refs/tags/*:refs/tags/*'
    if ($LASTEXITCODE -ne 0 -or !(HasCommit $mirror $Commit)) { throw "$Url has no commit $Commit" }
    return $mirror
}

try {
    $index = @("# profile`t$profile", "# name`tcommit`trepository`tpath")
    $names = @{}
    foreach ($line in $lockLines) {
        $trimmed = $line.Trim()
        if (!$trimmed -or $trimmed.StartsWith('#')) { continue }
        $fields = $trimmed -split '\s+'
        if ($fields.Count -ne 4) { throw "malformed lock line: $line" }
        $name, $url, $commit, $path = $fields
        if ($commit -notmatch '^[0-9a-f]{40}$') { throw "${name}: commit must be a full 40-digit hash" }
        if ($names.ContainsKey($name) -or $name -eq 'hmje-library' -or $name.Contains('/') -or $name.StartsWith('.')) {
            throw "invalid or duplicate platform library $name"
        }
        $names[$name] = $true
        $repo = [IO.Path]::GetFileNameWithoutExtension(($url.TrimEnd('/') -split '/')[-1])
        $extract = Join-Path $staging "extract-$name"
        New-Item -ItemType Directory -Force -Path $extract | Out-Null
        if ($profile -eq 'public' -and (Test-Path -LiteralPath $monorepo -PathType Container)) {
            $library = Join-Path $monorepo $name
            if ($path -ne '.' -or !(Test-Path -LiteralPath (Join-Path $library 'Project.hmjep') -PathType Leaf)) {
                throw "incomplete monorepo library: $name"
            }
            Copy-Item -Path (Join-Path $library '*') -Destination $extract -Recurse -Force
            $tree = $extract
        } else {
        $source = FindSource $repo $commit $url
        $archive = Join-Path $staging "$name.tar"
        if ($path -eq '.') { & git -C $source archive --format=tar -o $archive $commit }
        else {
            # Git for Windows validates the whole commit tree before applying
            # the path filter. Test fixtures outside this exported subtree may
            # have POSIX-only names; they never reach tar or the installed tree.
            & git -c core.protectNTFS=false -C $source archive --format=tar -o $archive $commit -- $path
        }
        if ($LASTEXITCODE -ne 0) { throw "git archive failed for $name" }
        & tar -x -f $archive -C $extract
        if ($LASTEXITCODE -ne 0) { throw "tar failed for $name" }
        $tree = if ($path -eq '.') { $extract } else { Join-Path $extract $path }
        }
        if (!(Test-Path -LiteralPath (Join-Path $tree 'Project.hmjep'))) { throw "$name has no Project.hmjep at $path" }
        foreach ($pruned in @('Tests', 'tests', 'docs', 'Examples', 'examples', 'scratch_check', 'Dependencies',
                              '.gitlab-ci.yml', '.gitignore', '.gitattributes', '.github')) {
            $target = Join-Path $tree $pruned
            if (Test-Path -LiteralPath $target) { Remove-Item -LiteralPath $target -Recurse -Force }
        }
        Get-ChildItem -LiteralPath $tree -Recurse -Force |
            Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint } |
            ForEach-Object { Remove-Item -LiteralPath $_.FullName -Force }
        Move-Item -LiteralPath $tree -Destination (Join-Path $out $name)
        $index += "$name`t$commit`t$url`t$path"
        [Console]::Error.WriteLine("vendored $name $commit")
    }
    [IO.File]::WriteAllText((Join-Path $out 'index.tsv'), (($index -join "`n") + "`n"), (New-Object Text.UTF8Encoding $false))
    if (Test-Path -LiteralPath $output) { Remove-Item -LiteralPath $output -Recurse -Force }
    Move-Item -LiteralPath $out -Destination $output
    Write-Output "platform libraries ($profile): $output"
} finally {
    if (Test-Path -LiteralPath $staging) { Remove-Item -LiteralPath $staging -Recurse -Force }
}

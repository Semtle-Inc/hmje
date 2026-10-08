param(
    # Versioned installation prefix. Default: %LOCALAPPDATA%\hmje\toolchains\<source revision>.
    [string]$Prefix,
    [Parameter(Mandatory = $true)][string]$NativeBinDirectory,
    # Do not make this version the per-user default (hmje-activate.ps1).
    [switch]$NoActivate,
    # Activate without adding %LOCALAPPDATA%\hmje\bin to the user PATH.
    [switch]$NoPath
)

$ErrorActionPreference = 'Stop'

# Windows PowerShell 5.1 cmdlets stop at MAX_PATH (260) unless the machine enables long
# paths; vendored platform libraries go deeper (Android maven trees) under a long
# %LOCALAPPDATA%. robocopy and `rd \\?\` handle long paths without that setting.
function CopyTree([string]$From, [string]$To) {
    $From = $From.Replace('/', '\'); $To = $To.Replace('/', '\')
    & robocopy.exe $From $To /E /COPY:DAT /DCOPY:T /R:1 /W:1 /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "copy failed ($LASTEXITCODE): $From -> $To" }
    $global:LASTEXITCODE = 0
}
function RemoveTree([string]$Path) {
    $Path = [IO.Path]::GetFullPath($Path).Replace('/', '\')
    if (Test-Path -LiteralPath $Path) {
        & cmd.exe /d /c "attrib -r `"\\?\$Path\*`" /s /d >nul 2>&1 & rd /s /q `"\\?\$Path`""
        if (Test-Path -LiteralPath $Path) { throw "could not remove $Path" }
    }
}
$source = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$native = [IO.Path]::GetFullPath($NativeBinDirectory)
$required = @('frontend.exe', 'backend.exe', 'product-coordinator.exe',
              'project-coordinator.exe', 'hmjec.exe')
foreach ($file in $required) {
    if (!(Test-Path -LiteralPath (Join-Path $native $file) -PathType Leaf)) {
        throw "Missing native binary: $file"
    }
}
$revisionFile = Join-Path $source 'SOURCE-REVISION'
$revisionText = if (Test-Path -LiteralPath $revisionFile) {
    (Get-Content -LiteralPath $revisionFile -TotalCount 1).Trim()
} else {
    $described = & git -C $source describe --always --dirty 2>$null
    if ($LASTEXITCODE -eq 0 -and $described) { "$described".Trim() } else { 'source' }
}
if (!$Prefix) {
    if (!$env:LOCALAPPDATA) { throw 'LOCALAPPDATA is not set; pass -Prefix' }
    $Prefix = Join-Path $env:LOCALAPPDATA "hmje\toolchains\$revisionText"
}
$prefixPath = [IO.Path]::GetFullPath($Prefix)
$parent = Split-Path -Parent $prefixPath
if (!$parent -or $parent -eq $prefixPath) { throw 'Choose a non-root installation prefix' }
$activate = Join-Path $source 'tools/windows/hmje-activate.ps1'
if (Test-Path -LiteralPath $prefixPath) {
    # Re-running the same install (same native binaries) only re-activates it.
    $existing = Join-Path $prefixPath 'lib/hmje/NATIVE-SHA256'
    $same = Test-Path -LiteralPath $existing
    if ($same) {
        $expected = foreach ($file in $required) {
            '{0}  {1}' -f (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $native $file)).Hash.ToLowerInvariant(), $file
        }
        $same = ((Get-Content -LiteralPath $existing) -join "`n") -eq ($expected -join "`n")
    }
    if (!$same) { throw "Installation destination already exists with different binaries: $prefixPath" }
    Write-Output "Already installed: $prefixPath"
    if (!$NoActivate) {
        & (Join-Path $prefixPath 'lib/hmje/bin/hmje-activate.ps1') -Prefix $prefixPath -NoPath:$NoPath
    }
    exit 0
}
if (!(Test-Path -LiteralPath $parent -PathType Container)) {
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
}
$staging = Join-Path $parent ('.i' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Path $staging | Out-Null
try {
    $payload = Join-Path $staging 'lib/hmje'
    New-Item -ItemType Directory -Force -Path (Join-Path $payload 'bin') | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $payload 'libexec/bin') | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $payload 'compiler/Sources') | Out-Null
    Copy-Item -LiteralPath (Join-Path $source 'bin/hmjec.ps1') -Destination (Join-Path $payload 'bin/hmjec.ps1')
    Copy-Item -LiteralPath (Join-Path $source 'bin/hmje-cache.ps1') -Destination (Join-Path $payload 'bin/hmje-cache.ps1')
    Copy-Item -LiteralPath $activate -Destination (Join-Path $payload 'bin/hmje-activate.ps1')
    foreach ($file in $required) {
        $destination = if ($file -eq 'hmjec.exe') { Join-Path $payload 'bin/hmjec.exe' }
                       else { Join-Path $payload "libexec/bin/$file" }
        Copy-Item -LiteralPath (Join-Path $native $file) -Destination $destination
    }
    CopyTree (Join-Path $source 'library') (Join-Path $payload 'library')
    CopyTree (Join-Path $source 'platforms') (Join-Path $payload 'platforms')
    foreach ($notice in @('LICENSE', 'LICENSE-RUNTIME', 'UNICODE-LICENSE.txt', 'THIRD_PARTY_NOTICES.md', 'SOURCES.md')) {
        $noticePath = Join-Path $source $notice
        if (Test-Path -LiteralPath $noticePath -PathType Leaf) {
            Copy-Item -LiteralPath $noticePath -Destination (Join-Path $payload $notice)
        }
    }
    # Platform libraries pinned by platform-libraries.lock ship with this toolchain.
    # build-windows-native.ps1 vendors them next to the binaries; otherwise vendor now.
    $vendored = Join-Path $native 'platform-libraries'
    if (Test-Path -LiteralPath (Join-Path $vendored 'index.tsv') -PathType Leaf) {
        CopyTree $vendored (Join-Path $payload 'platform-libraries')
    } else {
        & (Join-Path $source 'tools/vendor-platform-libraries.ps1') -OutputDirectory $payload | Out-Null
    }
    $indexLines = Get-Content -LiteralPath (Join-Path $payload 'platform-libraries/index.tsv') -Encoding UTF8
    if ($indexLines -contains "# profile`tpublic") {
        Remove-Item -LiteralPath (Join-Path $payload 'library/Internal') -Recurse -Force -ErrorAction SilentlyContinue
    }
    Copy-Item -LiteralPath (Join-Path $source 'platform-libraries.lock') -Destination (Join-Path $payload 'platform-libraries.lock')
    # Internal installs (index.tsv `# profile<TAB>internal`, HMJE_PLATFORM_PROFILE=internal) also record the private lock (docs/platform-libraries.md, public/internal profiles).
    $internalLock = Join-Path $source 'platform-libraries.internal.lock'
    if (($indexLines -contains "# profile`tinternal") -and (Test-Path -LiteralPath $internalLock -PathType Leaf)) {
        Copy-Item -LiteralPath $internalLock -Destination (Join-Path $payload 'platform-libraries.internal.lock')
    }
    & attrib.exe +r "\\?\$($payload.Replace('/', '\'))\platform-libraries\*" /s | Out-Null
    CopyTree (Join-Path $source 'compiler/runtime') (Join-Path $payload 'compiler/runtime')
    $prepare = [regex]::Unescape('\uC900\uBE44')
    $baseArray = [regex]::Unescape('\uAE30\uBCF8\uB3D9\uC801\uBC30\uC5F4') + '.hmje'
    New-Item -ItemType Directory -Force -Path (Join-Path $payload "compiler/Sources/$prepare") | Out-Null
    Copy-Item -LiteralPath (Join-Path $source "compiler/Sources/$prepare/$baseArray") `
              -Destination (Join-Path $payload "compiler/Sources/$prepare/$baseArray")
    $revisionText | Set-Content -LiteralPath (Join-Path $payload 'SOURCE-REVISION') -Encoding Ascii
    $checksums = foreach ($file in $required) {
        $item = if ($file -eq 'hmjec.exe') { Join-Path $payload 'bin/hmjec.exe' }
                else { Join-Path $payload "libexec/bin/$file" }
        '{0}  {1}' -f (Get-FileHash -Algorithm SHA256 -LiteralPath $item).Hash.ToLowerInvariant(), $file
    }
    $checksums | Set-Content -LiteralPath (Join-Path $payload 'NATIVE-SHA256') -Encoding Ascii
    New-Item -ItemType Directory -Force -Path (Join-Path $staging 'bin') | Out-Null
    @('@echo off', '"%~dp0..\lib\hmje\bin\hmjec.exe" %*', 'exit /b %ERRORLEVEL%') |
        Set-Content -LiteralPath (Join-Path $staging 'bin/hmjec.cmd') -Encoding Ascii
    Move-Item -LiteralPath $staging -Destination $prefixPath
    Write-Output "Installed $prefixPath\bin\hmjec.cmd (Windows native toolchain)"
    if (!$NoActivate) {
        & (Join-Path $prefixPath 'lib/hmje/bin/hmje-activate.ps1') -Prefix $prefixPath -NoPath:$NoPath
    }
    # Warm this user's shared cache with every platform library (HMJE_INSTALL_WARM=0 skips).
    # It needs LLVM clang and the MSVC/Windows SDK libraries (the same prerequisites as a build).
    if ($env:HMJE_INSTALL_WARM -ne '0') {
        & (Join-Path $prefixPath 'lib/hmje/bin/hmjec.exe') --warm-platform-libraries
        if ($LASTEXITCODE -ne 0) { Write-Warning 'platform library warm-up failed; first builds will fill the cache instead' }
    }
} finally {
    RemoveTree $staging
}

$CommandArguments = [string[]]$args

$ErrorActionPreference = 'Stop'
function Fail([string]$Message) { throw $Message }
function NativePath([string]$Value) {
    return [IO.Path]::GetFullPath($Value).Replace('\', '/')
}
function RunNative([string]$Program, [string[]]$NativeArguments) {
    & $Program @NativeArguments
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
function NeedValue([int]$Index, [string]$Option) {
    if ($Index + 1 -ge $CommandArguments.Count -or !$CommandArguments[$Index + 1]) {
        Fail "$Option needs a value"
    }
    return $CommandArguments[$Index + 1]
}

try {
    $root = NativePath (Join-Path $PSScriptRoot '..')
    $bin = if ($env:HMJE_TOOL_BIN) { NativePath $env:HMJE_TOOL_BIN } else { "$root/libexec/bin" }
    $frontend = if ($env:HMJE_FRONTEND) { NativePath $env:HMJE_FRONTEND } else { "$bin/frontend.exe" }
    $backend = if ($env:HMJE_BACKEND) { NativePath $env:HMJE_BACKEND } else { "$bin/backend.exe" }
    $product = if ($env:HMJE_COORDINATOR) { NativePath $env:HMJE_COORDINATOR } else { "$bin/product-coordinator.exe" }
    $projectCoordinator = if ($env:HMJE_PROJECT_COORDINATOR) { NativePath $env:HMJE_PROJECT_COORDINATOR } else { "$bin/project-coordinator.exe" }
    $self = NativePath (Join-Path $PSScriptRoot 'hmjec.exe')
    $cacheBase = if ($env:HMJE_CACHE_HOME) { NativePath $env:HMJE_CACHE_HOME } elseif ($env:LOCALAPPDATA) {
        NativePath (Join-Path $env:LOCALAPPDATA 'hmje')
    } else { Fail 'LOCALAPPDATA or HMJE_CACHE_HOME is required' }
    $cache = if ($env:HMJE_CACHE_ROOT) { NativePath (Split-Path -Parent $env:HMJE_CACHE_ROOT) } else { "$cacheBase/v1" }
    if ($CommandArguments.Count -eq 0) { Fail 'usage: hmjec --project ROOT --artifact NAME --target windows-exe -o OUTPUT' }
    if ($CommandArguments[0] -eq 'cache') {
        if ($CommandArguments.Count -lt 2 -or $CommandArguments[1] -ne 'prune') {
            Fail 'usage: hmjec cache prune [--max-gib N] [--min-age-hours N]'
        }
        # Unset limits follow the shared policy (HMJE_CACHE_* variables, see hmje-cache.ps1).
        $pruneArguments = @{ Root = $cache }
        for ($index = 2; $index -lt $CommandArguments.Count; $index++) {
            switch ($CommandArguments[$index]) {
                '--max-gib' { $pruneArguments.MaxGiB = [long](NeedValue $index '--max-gib'); $index++; break }
                '--min-age-hours' { $pruneArguments.MinAgeMinutes = 60 * [long](NeedValue $index '--min-age-hours'); $index++; break }
                '--min-age-minutes' { $pruneArguments.MinAgeMinutes = [long](NeedValue $index '--min-age-minutes'); $index++; break }
                default { Fail "unknown cache prune option: $($CommandArguments[$index])" }
            }
        }
        & (Join-Path $PSScriptRoot 'hmje-cache.ps1') @pruneArguments
        exit 0
    }
    if ($CommandArguments[0] -eq '--help' -or $CommandArguments[0] -eq '-h') {
        Write-Output 'hmjec --project ROOT --artifact NAME --target windows-exe -o OUTPUT [--cache-dir DIR] [--jobs N]'
        Write-Output 'hmjec --target windows-exe|windows-llvm-ir|android-llvm-ir|android-exe|android-apk SOURCE -o OUTPUT [--target-arch aarch64|x86_64]'
        Write-Output 'hmjec --platform-libraries   (bundled platform libraries and pinned commits; project builds accept --verbose)'
        exit 0
    }
    if ($CommandArguments[0] -eq '--platform-libraries' -or
        $CommandArguments[0] -eq [regex]::Unescape('--\uD50C\uB7AB\uD3FC\uB77C\uC774\uBE0C\uB7EC\uB9AC')) {
        if ($CommandArguments.Count -ne 1) { Fail "usage: hmjec $($CommandArguments[0])" }
        RunNative $projectCoordinator @('--platform-libraries', $self)
        exit 0
    }
    if ($CommandArguments[0] -eq '--warm-platform-libraries' -or
        $CommandArguments[0] -eq [regex]::Unescape('--\uD50C\uB7AB\uD3FC\uB77C\uC774\uBE0C\uB7EC\uB9AC\uC608\uC5F4')) {
        if ($CommandArguments.Count -ne 1) { Fail "usage: hmjec $($CommandArguments[0])" }
        $warm = NativePath (Join-Path ([IO.Path]::GetTempPath()) ('hmje-platform-warm-' + [guid]::NewGuid().ToString('N')))
        try {
            $projects = & $projectCoordinator '--platform-warm-project' $self $warm
            if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
            $artifactName = [regex]::Unescape('\uC608\uC5F4')
            $failed = $false
            foreach ($project in @($projects | Where-Object { $_ })) {
                # Windows PowerShell 5.1 turns redirected native stderr (clang warnings) into
                # error records; under 'Stop' the first warning would abort the warm-up.
                # Compile the common UI/base/std shapes through the native link so their
                # content-keyed COFF partitions and runtime objects are reusable by new
                # projects. The HMJE coordinator selects IR-only warm-up for the
                # all-libraries shape, which can include foreign native dependencies.
                $warmPhase = Get-Content -LiteralPath "$project/warm-target" -Raw
                $warmTarget = "windows-$warmPhase"
                $warmOutput = if ($warmTarget -eq 'windows-exe') { "$project/warm.exe" } else { "$project/warm.ll" }
                $ErrorActionPreference = 'Continue'
                & $self --project $project --artifact $artifactName --target $warmTarget -o $warmOutput *> "$project.log"
                $warmCode = $LASTEXITCODE
                $ErrorActionPreference = 'Stop'
                if ($warmCode -ne 0) {
                    Get-Content -LiteralPath "$project.log" -Tail 20 | ForEach-Object { [Console]::Error.WriteLine($_) }
                    $failed = $true
                }
            }
            if ($failed) { Fail 'platform library warm-up failed; first builds will fill the rest of the cache' }
            Write-Output "platform library cache warmed: $cache"
        } finally {
            if ($env:HMJE_KEEP_TEMP -eq '1') {
                [Console]::Error.WriteLine("HMJE_KEEP_TEMP=1: $warm")
            } else {
                Remove-Item -LiteralPath $warm -Recurse -Force -ErrorAction SilentlyContinue
            }
        }
        exit 0
    }
    if ($CommandArguments[0] -eq '--version') {
        $revision = Join-Path $root 'SOURCE-REVISION'
        if (Test-Path -LiteralPath $revision) { Write-Output "hmjec native $((Get-Content -LiteralPath $revision -TotalCount 1))" }
        else { Write-Output 'hmjec native development' }
        $platformIndex = Join-Path $root 'platform-libraries/index.tsv'
        if (Test-Path -LiteralPath $platformIndex) {
            $count = @(Get-Content -LiteralPath $platformIndex | Where-Object { $_ -and !$_.StartsWith('#') }).Count
            Write-Output ("platform libraries: $count + toolchain library (hmjec --platform-libraries)")
        }
        exit 0
    }
    $projectMode = $false
    $target = ''; $source = ''; $output = ''; $projectRoot = ''; $artifact = ''
    $moduleMap = ''; $nativePlan = ''; $libraryRoot = "$root/library"
    $targetArch = ''; $noCache = $false; $measure = $false; $dumpDeps = $false
    $compilerToolBuild = $false
    $defines = @(); $jobs = ''; $extra = @(); $cacheExplicit = $false
    $cacheOffFlag = [regex]::Unescape('--\uCE90\uC2DC\uB044\uAE30')
    $measureFlag = [regex]::Unescape('--\uACC4\uCE21')
    $verboseFlag = [regex]::Unescape('--\uC790\uC138\uD788')
    for ($i = 0; $i -lt $CommandArguments.Count; $i++) {
        $arg = $CommandArguments[$i]
        if ($arg -eq $cacheOffFlag) { $noCache = $true; continue }
        if ($arg -eq $measureFlag) { $measure = $true; continue }
        if ($arg -eq $verboseFlag -or $arg -eq '--verbose' -or $arg -eq '-v') { $env:HMJE_PROJECT_VERBOSE = '1'; continue }
        switch ($arg) {
            '--project' { $projectMode = $true; $projectRoot = NeedValue $i $arg; $i++; break }
            '--artifact' { $artifact = NeedValue $i $arg; $i++; break }
            '--target' { $target = NeedValue $i $arg; $i++; break }
            '-o' { $output = NeedValue $i $arg; $i++; break }
            '--output' { $output = NeedValue $i $arg; $i++; break }
            '--cache-dir' { $cache = NativePath (NeedValue $i $arg); $cacheExplicit = $true; $i++; break }
            '--cache-root' { $cache = NativePath (NeedValue $i $arg); $cacheExplicit = $true; $i++; break }
            '--module-map' { $moduleMap = NativePath (NeedValue $i $arg); $i++; break }
            '--native-plan' { $nativePlan = NativePath (NeedValue $i $arg); $i++; break }
            '--library-root' { $libraryRoot = NativePath (NeedValue $i $arg); $i++; break }
            '--android-min-sdk' { $env:HMJE_ANDROID_MIN_SDK = NeedValue $i $arg; $i++; break }
            '--target-arch' { $targetArch = NeedValue $i $arg; $i++; break }
            '--platform' {
                $platform = NeedValue $i $arg
                if ($platform -notin @('windows', 'Windows')) { Fail "unsupported Windows host platform: $platform" }
                $i++; break
            }
            '--jobs' { $jobs = NeedValue $i $arg; $i++; break }
            '-j' { $jobs = NeedValue $i $arg; $i++; break }
            '--define' { $defines += (NeedValue $i $arg); $i++; break }
            '--no-cache' { $noCache = $true; break }
            '--trusted-compiler-build' { $compilerToolBuild = $true; break }
            '--dump-deps' { $dumpDeps = $true; break }
            '--llvm-ir' { $extra += @($arg, (NativePath (NeedValue $i $arg))); $i++; break }
            '--os-root' { $extra += @($arg, (NativePath (NeedValue $i $arg))); $i++; break }
            '--ui-root' { $env:HMJE_UI_ROOT = NativePath (NeedValue $i $arg); $i++; break }
            default {
                if ($arg.StartsWith('--')) { Fail "unsupported Windows hmjec option: $arg" }
                if ($source) { Fail 'multiple source files are not supported' }
                $source = $arg
            }
        }
    }
    if (!$target -or !$output) { Fail '--target and -o are required' }
    if ($target -notin @('windows-exe', 'windows-llvm-ir', 'android-llvm-ir', 'android-exe', 'android-apk')) { Fail "unsupported Windows host target: $target" }
    if ($targetArch -and $targetArch -notin @('aarch64', 'x86_64', 'arm64')) { Fail "unsupported architecture: $targetArch" }
    if ($targetArch -eq 'arm64') { $targetArch = 'aarch64' }
    $android = $target.StartsWith('android-')
    if ($android -and $targetArch -and $targetArch -ne 'aarch64') { Fail 'Android currently requires --target-arch aarch64 (arm64-v8a)' }
    if ($android) { $targetArch = 'aarch64' }
    if (!$targetArch) { $targetArch = if ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64') { 'aarch64' } else { 'x86_64' } }
    if ($jobs) {
        if ($jobs -notmatch '^[0-9]+$' -or [int]$jobs -lt 1 -or [int]$jobs -gt 64) { Fail '--jobs must be 1..64' }
        $env:HMJE_JOBS = $jobs
    }
    $platform = 'windows'
    if ($android) {
        $platform = 'android'
        $minimum = if ($env:HMJE_ANDROID_MIN_SDK) { $env:HMJE_ANDROID_MIN_SDK } else { '33' }
        if ($minimum -notmatch '^[0-9]+$' -or [int]$minimum -lt 21) { Fail 'Android arm64 requires API 21 or later' }
        $env:HMJE_ANDROID_MIN_SDK = $minimum
        $triple = "aarch64-linux-android$minimum"
        $clang = 'clang.exe'
    } else {
        $triple = "$targetArch-pc-windows-msvc"
        if ($env:HMJE_WINDOWS_TRIPLE -and $env:HMJE_WINDOWS_TRIPLE -ne $triple) { Fail 'target architecture conflicts with HMJE_WINDOWS_TRIPLE' }
        $env:HMJE_WINDOWS_TRIPLE = $triple
        $env:HMJE_WINDOWS_LINK_MODE = 'local'
        $clang = if ($env:HMJE_WINDOWS_CLANG) { NativePath $env:HMJE_WINDOWS_CLANG } else {
            $resolved = Get-Command clang.exe -ErrorAction SilentlyContinue
            if (!$resolved) { Fail 'LLVM clang.exe is required on Windows' }
            NativePath $resolved.Source
        }
        if (!(Test-Path -LiteralPath $clang -PathType Leaf)) { Fail "clang.exe is missing: $clang" }
        $env:HMJE_WINDOWS_CLANG = $clang
        if (!$env:HMJE_WINDOWS_IR_CC) { $env:HMJE_WINDOWS_IR_CC = $clang }
        if ($env:HMJE_WINDOWS_IR_CC -eq 'guest') { Fail 'guest IR compiler is not valid in a standalone Windows installation' }
    }
    $output = NativePath $output
    $outputParent = Split-Path -Parent $output
    New-Item -ItemType Directory -Force -Path $outputParent | Out-Null
    New-Item -ItemType Directory -Force -Path $cache | Out-Null
    if ($cacheExplicit -or !$env:HMJE_PARSE_CACHE) { $env:HMJE_PARSE_CACHE = "$cache/parse" }
    $env:HMJE_CACHE_ROOT = "$cache/products"
    $env:HMJE_COMPILER_HOST_FILES = if ($compilerToolBuild) { '1' } else { '' }
    New-Item -ItemType Directory -Force -Path $env:HMJE_PARSE_CACHE | Out-Null
    New-Item -ItemType Directory -Force -Path $env:HMJE_CACHE_ROOT | Out-Null
    if (!$noCache -and $env:HMJE_CACHE_GC -ne 'off' -and !$cacheExplicit) {
        & (Join-Path $PSScriptRoot 'hmje-cache.ps1') $cache -Automatic
    }
    if ($projectMode) {
        if (!$projectRoot -or !$artifact -or $source) { Fail 'project mode needs --project ROOT and --artifact NAME' }
        $projectRoot = NativePath $projectRoot
        if ((Split-Path -Leaf $projectRoot) -eq 'Project.hmjep') { $projectRoot = NativePath (Split-Path -Parent $projectRoot) }
        if (!$env:HMJE_HOST_SUPPORT -and ($compilerToolBuild -or $projectRoot -eq (NativePath "$root/compiler"))) {
            $compilerSupport = "$projectRoot/runtime/host_support_windows.c"
            if (!(Test-Path -LiteralPath $compilerSupport -PathType Leaf)) { Fail 'compiler host support is missing from the compiler project' }
            $env:HMJE_HOST_SUPPORT = $compilerSupport
        }
        if (!(Test-Path -LiteralPath "$projectRoot/Project.hmjep" -PathType Leaf)) { Fail 'Project.hmjep is missing' }
        $scratch = NativePath (Join-Path $cache ("invocation.$PID.$([guid]::NewGuid().ToString('N'))"))
        New-Item -ItemType Directory -Path $scratch | Out-Null
        $env:HMJE_PROJECT_SCRATCH = $scratch
        $env:HMJE_PROJECT_SOURCE_ID = "$projectRoot|$artifact|$target|$targetArch"
        $env:HMJE_PROJECT_SOURCE_PATH = "$scratch/entry.hmje"
        try {
            $call = @($self, $projectRoot, $artifact, $target, $output, $cache, $(if ($android) { 'Android' } else { 'Windows' }))
            if ($noCache) { $call += $cacheOffFlag }
            if ($dumpDeps) { $call += '--dump-deps' }
            if ($measure) { $call += $measureFlag }
            if ($defines.Count) { $call += @('--define', ($defines -join "`n")) }
            if ($targetArch) { $call += @('--target-arch', $targetArch) }
            if ($compilerToolBuild) { $call += '--trusted-compiler-build' }
            $call += $extra
            RunNative $projectCoordinator $call
            if (!$dumpDeps -and !(Test-Path -LiteralPath $output -PathType Leaf)) { Fail "compiler returned success without output: $output" }
        } finally {
            if ($env:HMJE_KEEP_TEMP -eq '1') {
                [Console]::Error.WriteLine("HMJE_KEEP_TEMP=1: $scratch")
            } else {
                Remove-Item -LiteralPath $scratch -Recurse -Force -ErrorAction SilentlyContinue
            }
        }
    } else {
        if (!$source) { Fail 'source is required' }
        $source = NativePath $source
        if (!(Test-Path -LiteralPath $source -PathType Leaf)) { Fail "source is missing: $source" }
        $env:HMJE_BUILD_HOST_ARCH = $targetArch
        $kind = if ($target -eq 'windows-exe') { 'windows-exe' } elseif ($target -eq 'android-llvm-ir') { 'android-llvm' } elseif ($android) { $target } else { 'llvm' }
        $base = "$root/compiler/Sources/" + [regex]::Unescape('\uC900\uBE44') + '/' + [regex]::Unescape('\uAE30\uBCF8\uB3D9\uC801\uBC30\uC5F4') + '.hmje'
        $hostSupport = if ($env:HMJE_HOST_SUPPORT) { $env:HMJE_HOST_SUPPORT } else { "$root/compiler/runtime/host_app_windows.c" }
        foreach ($required in @($frontend, $backend, $product, $base, $hostSupport)) {
            if (!(Test-Path -LiteralPath $required -PathType Leaf)) { Fail "missing installed compiler file: $required" }
        }
        $call = @($frontend, $backend, $source, $libraryRoot, $platform, $base,
                  $env:HMJE_CACHE_ROOT, $output, $triple, $kind, $root, $hostSupport,
                  $targetArch, $clang)
        if ($moduleMap) { $call += @('--module-map', $moduleMap) }
        if ($nativePlan) { $call += @('--native-plan', $nativePlan) }
        if ($noCache) { $call += $cacheOffFlag }
        if ($measure) { $call += $measureFlag }
        if ($dumpDeps) { $call += '--dump-deps' }
        $call += $extra
        RunNative $product $call
        if (!$dumpDeps -and !(Test-Path -LiteralPath $output -PathType Leaf)) { Fail "compiler returned success without output: $output" }
    }
} catch {
    [Console]::Error.WriteLine("hmjec: $($_.Exception.Message)")
    exit 2
}

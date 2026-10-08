param(
    [Parameter(Mandatory = $true)][string]$Hmjec,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [ValidateSet('aarch64', 'x86_64')][string]$TargetArch = 'aarch64'
)

# Rebuild all compiler executables on Windows using an already installed native
# toolchain. The installed compiler is the seed, not a runtime dependency of the
# binaries produced here; no Mac host, POSIX shell, or Parallels API is involved.
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$hmjecPath = [IO.Path]::GetFullPath($Hmjec)
$output = [IO.Path]::GetFullPath($OutputDirectory)
if (!(Test-Path -LiteralPath $hmjecPath -PathType Leaf)) { throw "hmjec not found: $hmjecPath" }
New-Item -ItemType Directory -Force -Path $output | Out-Null
$compiler = Join-Path $root 'compiler'
# The pinned platform libraries are part of the distributable toolchain; vendor them
# first so a missing commit fails before the long native build.
& (Join-Path $root 'tools/vendor-platform-libraries.ps1') -OutputDirectory $output | Out-Null
if (!$env:HMJE_WINDOWS_IR_OPT_LEVEL) { $env:HMJE_WINDOWS_IR_OPT_LEVEL = '1' }
$tools = @(
    @([regex]::Unescape('hmir\uc744llvm\uc73c\ub85c'), 'backend.exe'),
    @([regex]::Unescape('\uc81c\ud488\uc870\uc815\uae30'), 'product-coordinator.exe'),
    @([regex]::Unescape('\ud504\ub85c\uc81d\ud2b8\uc870\uc815\uae30'), 'project-coordinator.exe'),
    @([regex]::Unescape('\ucef4\ud30c\uc77c\ub7ec'), 'frontend.exe')
)
# Windows PowerShell 5.1 promotes redirected native stderr to NativeCommandError.
# LLVM may warn and still exit successfully; preserve its diagnostic and use the
# actual process exit code, including when this script is captured in a build log.
function InvokeNative([string]$Program, [string[]]$Arguments, [string]$Failure) {
    $savedPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & $Program @Arguments
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $savedPreference
    }
    if ($code -ne 0) { throw "$Failure ($code)" }
}
# Released seeds may add window/GPU libraries to headless compiler links.
# Bootstrap the current coordinator before producing any distributable tool.
$bootstrap = Join-Path $output '.bootstrap-tools'
New-Item -ItemType Directory -Force -Path $bootstrap | Out-Null
foreach ($tool in $tools[0..1]) {
    $destination = Join-Path $bootstrap $tool[1]
    InvokeNative $hmjecPath @('--project', $compiler, '--artifact', $tool[0], '--target', 'windows-exe',
        '--target-arch', $TargetArch, '--trusted-compiler-build', '-o', $destination) "Bootstrap compiler build failed: $($tool[0])"
    if ($tool[1] -eq 'backend.exe') { $env:HMJE_BACKEND = $destination }
    if ($tool[1] -eq 'product-coordinator.exe') { $env:HMJE_COORDINATOR = $destination }
}
foreach ($tool in $tools) {
    $artifact = $tool[0]
    $destination = Join-Path $output $tool[1]
    InvokeNative $hmjecPath @('--project', $compiler, '--artifact', $artifact, '--target', 'windows-exe',
        '--target-arch', $TargetArch, '--trusted-compiler-build', '-o', $destination) "Native compiler build failed: $artifact"
    if ($tool[1] -eq 'backend.exe') { $env:HMJE_BACKEND = $destination }
    if ($tool[1] -eq 'product-coordinator.exe') { $env:HMJE_COORDINATOR = $destination }
    if ($tool[1] -eq 'project-coordinator.exe') { $env:HMJE_PROJECT_COORDINATOR = $destination }
}
$clang = if ($env:HMJE_WINDOWS_CLANG) { $env:HMJE_WINDOWS_CLANG } else {
    $resolved = Get-Command clang.exe -ErrorAction SilentlyContinue
    if (!$resolved) { throw 'LLVM clang.exe is required' }
    $resolved.Source
}
InvokeNative $clang @('-target', "$TargetArch-pc-windows-msvc", '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
    (Join-Path $root 'tools/windows/hmjec_launcher.c'), '-lshell32', '-o', (Join-Path $output 'hmjec.exe')) 'Native hmjec launcher build failed'
Remove-Item -LiteralPath $bootstrap -Recurse -Force
Write-Output "Windows native compiler binaries: $output"

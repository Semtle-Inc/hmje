param(
    # Toolchain prefix to activate (default: the installation holding this script).
    [string]$Prefix,
    # Per-user activation home (default %LOCALAPPDATA%\hmje): bin\hmjec.exe + hmjec.ini.
    [string]$HmjeHome,
    # Leave the user PATH alone.
    [switch]$NoPath,
    # Undo activation: remove HmjeHome\bin and its user PATH entry.
    [switch]$Remove
)

# Per-user activation of an installed Windows toolchain. No administrator rights:
# only %LOCALAPPDATA% and the current user's PATH (HKCU\Environment) change.
# HmjeHome\bin\hmjec.exe is a copy of the native launcher; hmjec.ini beside it names
# the active version's hmjec.ps1, so installing another version rewrites one line.
$ErrorActionPreference = 'Stop'
if (!$HmjeHome) {
    if (!$env:LOCALAPPDATA) { throw 'LOCALAPPDATA is not set; pass -HmjeHome' }
    $HmjeHome = Join-Path $env:LOCALAPPDATA 'hmje'
}
$HmjeHome = [IO.Path]::GetFullPath($HmjeHome)
$bin = Join-Path $HmjeHome 'bin'

function UserPathEntries {
    $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Environment', $true)
    $raw = $key.GetValue('Path', '', [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
    return @($key, [string]$raw)
}

function SamePath([string]$A, [string]$B) {
    $x = [Environment]::ExpandEnvironmentVariables($A).TrimEnd('\')
    $y = [Environment]::ExpandEnvironmentVariables($B).TrimEnd('\')
    return [string]::Equals($x, $y, [StringComparison]::OrdinalIgnoreCase)
}

function Broadcast {
    # SetEnvironmentVariable(User) broadcasts WM_SETTINGCHANGE so new shells see PATH.
    [Environment]::SetEnvironmentVariable('HMJE_HOME', $HmjeHome, 'User')
}

if ($Remove) {
    $key, $raw = UserPathEntries
    $kept = @($raw -split ';' | Where-Object { $_ -and !(SamePath $_ $bin) })
    $key.SetValue('Path', ($kept -join ';'), [Microsoft.Win32.RegistryValueKind]::ExpandString)
    $key.Close()
    [Environment]::SetEnvironmentVariable('HMJE_HOME', $null, 'User')
    if (Test-Path -LiteralPath $bin) { Remove-Item -LiteralPath $bin -Recurse -Force }
    Write-Output "hmje activation removed: $bin"
    exit 0
}

if (!$Prefix) { $Prefix = Join-Path $PSScriptRoot '..\..\..' }
$Prefix = [IO.Path]::GetFullPath($Prefix)
$script = Join-Path $Prefix 'lib\hmje\bin\hmjec.ps1'
$launcher = Join-Path $Prefix 'lib\hmje\bin\hmjec.exe'
foreach ($required in @($script, $launcher)) {
    if (!(Test-Path -LiteralPath $required -PathType Leaf)) { throw "not an hmje toolchain prefix: $Prefix (missing $required)" }
}
New-Item -ItemType Directory -Force -Path $bin | Out-Null
# Replace the launcher copy atomically; a running hmjec.exe keeps the old image.
$staged = Join-Path $bin ('.hmjec-' + [guid]::NewGuid().ToString('N') + '.exe')
Copy-Item -LiteralPath $launcher -Destination $staged
try {
    Move-Item -LiteralPath $staged -Destination (Join-Path $bin 'hmjec.exe') -Force
} catch {
    Remove-Item -LiteralPath $staged -Force -ErrorAction SilentlyContinue
    if (!(Test-Path -LiteralPath (Join-Path $bin 'hmjec.exe'))) { throw }
    Write-Warning "hmjec.exe is in use; keeping the existing launcher copy ($($_.Exception.Message))"
}
$utf8 = New-Object Text.UTF8Encoding $false
[IO.File]::WriteAllText((Join-Path $bin 'hmjec.ini'), $script + "`r`n", $utf8)
[IO.File]::WriteAllText((Join-Path $HmjeHome 'current.txt'), $Prefix + "`r`n", $utf8)
if (!$NoPath) {
    $key, $raw = UserPathEntries
    $entries = @($raw -split ';' | Where-Object { $_ })
    if (!($entries | Where-Object { SamePath $_ $bin })) {
        $key.SetValue('Path', ((@($bin) + $entries) -join ';'), [Microsoft.Win32.RegistryValueKind]::ExpandString)
    }
    $key.Close()
    Broadcast
}
Write-Output "hmjec active: $bin\hmjec.exe -> $Prefix"

param([Parameter(Mandatory=$true)][string]$OutputDirectory,[string]$Clang='clang.exe')
$ErrorActionPreference='Stop'
$source=Join-Path $PSScriptRoot 'probe.c'
New-Item -ItemType Directory -Force -Path $OutputDirectory|Out-Null
$previous=@{}
foreach($key in @('HOME','USERPROFILE','LOCALAPPDATA','HMJE_WINDOWS_SCALE')){$previous[$key]=[Environment]::GetEnvironmentVariable($key,'Process')}
try {
 foreach($arch in @('aarch64','x86_64')) {
  $exe=Join-Path $OutputDirectory ('probe-'+$arch+'.exe')
  & $Clang -target ($arch+'-pc-windows-msvc') -O2 $source -o $exe -ldxguid
  if($LASTEXITCODE -ne 0){throw ('probe build failed: '+$arch)}
  foreach($scale in @('1','2')) {
   $homePath=Join-Path $OutputDirectory ('home-'+$arch+'-'+$scale+'-'+[Guid]::NewGuid().ToString('N'))
   $env:HOME=$homePath;$env:USERPROFILE=$homePath;$env:LOCALAPPDATA=Join-Path $homePath 'AppData/Local';$env:HMJE_WINDOWS_SCALE=$scale
   New-Item -ItemType Directory -Force -Path $env:LOCALAPPDATA|Out-Null
   $log=Join-Path $OutputDirectory ('probe-'+$arch+'-'+$scale+'.log')
   $ErrorActionPreference='Continue'
   & $exe *> $log
   $code=$LASTEXITCODE;$ErrorActionPreference='Stop'
   Get-Content $log
   if($code -ne 0){throw ('probe failed: '+$arch+' scale '+$scale)}
  }
 }
} finally {foreach($key in $previous.Keys){[Environment]::SetEnvironmentVariable($key,$previous[$key],'Process')}}

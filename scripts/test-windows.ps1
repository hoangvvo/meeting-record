param(
  [ValidateSet("x64", "arm64")]
  [string]$Arch = $(if ($env:PROCESSOR_ARCHITECTURE -eq "ARM64") { "arm64" } else { "x64" })
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root "native/build/tests-windows-$Arch"
New-Item -ItemType Directory -Force $build | Out-Null

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
  $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio/Installer/vswhere.exe"
  if (-not (Test-Path $vswhere)) { throw "Visual Studio C++ build tools are required" }
  $installation = & $vswhere -latest -products * -property installationPath
  if (-not $installation) { throw "Visual Studio C++ build tools are required" }
  $vcvars = Join-Path $installation "VC/Auxiliary/Build/vcvarsall.bat"
  $target = if ($Arch -eq "arm64") { "arm64" } else { "amd64" }
  # `call` keeps the quoted path intact under both Windows PowerShell and pwsh.
  $variables = & cmd.exe /d /c "call `"$vcvars`" $target >nul && set"
  if ($LASTEXITCODE -ne 0) { throw "Could not initialize the Visual Studio toolchain" }
  foreach ($variable in $variables) {
    if ($variable -match '^([^=]+)=(.*)$') {
      [Environment]::SetEnvironmentVariable($matches[1], $matches[2], "Process")
    }
  }
}

function Test-Native {
  param([string]$Name, [string[]]$Sources, [switch]$Cxx)

  Write-Host "==> $Name"
  # @() keeps one flag an array; Windows PowerShell would splat a string per character.
  $language = @(if ($Cxx) { "/std:c++17", "/EHsc" } else { "/std:c11" })
  & cl.exe /nologo /W4 /utf-8 /D_CRT_SECURE_NO_WARNINGS @language `
    "/I$root/native/include" "/I$root/native/common" "/Fe:$Name.exe" @Sources
  if ($LASTEXITCODE -ne 0) { throw "$Name compilation failed" }
  & ".\$Name.exe"
  if ($LASTEXITCODE -ne 0) { throw "$Name failed" }
}

Push-Location $build
try {
  $vendor = Join-Path $root "native/third_party/speexdsp"
  foreach ($source in @("mdf", "preprocess", "filterbank", "fftwrap", "kiss_fft", "kiss_fftr")) {
    & cl.exe /nologo /std:c11 /O2 /DHAVE_CONFIG_H "/I$vendor" "/I$vendor/include" /c "$vendor/libspeexdsp/$source.c"
    if ($LASTEXITCODE -ne 0) { throw "voice DSP compilation failed: $source" }
  }
  & cl.exe /nologo /std:c++17 /EHsc /W4 "/I$vendor" "/I$vendor/include" `
    "$root/native/tests/voice_processing_test.cpp" "$root/native/common/voice-processing.cpp" `
    mdf.obj preprocess.obj filterbank.obj fftwrap.obj kiss_fft.obj kiss_fftr.obj /Fe:voice_processing_test.exe
  if ($LASTEXITCODE -ne 0) { throw "voice processor compilation failed" }
  & .\voice_processing_test.exe
  if ($LASTEXITCODE -ne 0) { throw "voice processing tests failed" }
  Test-Native layout_check @("$root/native/tests/layout_check.c")
  Test-Native pcm_mixer_test @("$root/native/tests/pcm_mixer_test.cpp", "$root/native/common/pcm-mixer.cpp") -Cxx
  Test-Native meeting_catalog_test @(
    "$root/native/tests/meeting_catalog_test.c", "$root/native/common/meeting-catalog.c"
  )
  Test-Native tracks_test @("$root/native/tests/tracks_test.c", "$root/native/common/tracks.c")
  Test-Native tracks_concurrency_test @("$root/native/tests/tracks_concurrency_test.cpp", "tracks.obj") -Cxx
  Test-Native capture_pump_test @("$root/native/tests/capture_pump_test.cpp", "tracks.obj") -Cxx
  Test-Native audio_app_process_test @("$root/native/tests/audio_app_process_test.cpp") -Cxx
  Test-Native timeline_mixer_test @("$root/native/tests/timeline_mixer_test.cpp") -Cxx
  Test-Native capture_roots_test @("$root/native/tests/capture_roots_test.cpp") -Cxx
  Test-Native audio_session_notifications_test @(
    "$root/native/tests/audio_session_notifications_test.cpp", "ole32.lib", "avrt.lib"
  ) -Cxx
} finally {
  Pop-Location
}

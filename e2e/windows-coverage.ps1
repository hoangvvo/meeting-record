# Category/endpoint coverage through the native backend and both bindings.
# Builds first, plays audible tones, and restores the communications output in finally.
[CmdletBinding()]
param(
    [ValidateSet("Rust", "Node", "All")][string]$Binding = "All",
    [ValidateRange(3, 30)][int]$DurationSeconds = 4,
    [string]$SeparateOutput,
    [switch]$EndpointReference,
    [switch]$AllowSystemAudioFallback,
    [switch]$CompatibilityTarget,
    [switch]$SkipBuild
)
$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest
$root = Split-Path -Parent $PSScriptRoot
$devices = Join-Path $PSScriptRoot "windows-audio-device.ps1"
$build = Join-Path $root "native/build/windows-coverage"
New-Item -ItemType Directory -Force $build | Out-Null

function Invoke-Checked([scriptblock]$Command) {
    $ErrorActionPreference = "Continue"
    & $Command 2>&1 | ForEach-Object { "$_" }
    if ($LASTEXITCODE -ne 0) { throw "command failed with exit code $LASTEXITCODE" }
}

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio/Installer/vswhere.exe"
    $installation = & $vswhere -latest -products * -property installationPath
    if (-not $installation) { throw "Visual Studio C++ build tools are required" }
    $vcvars = Join-Path $installation "VC/Auxiliary/Build/vcvarsall.bat"
    $variables = & cmd.exe /d /c "call `"$vcvars`" amd64 >nul && set"
    if ($LASTEXITCODE -ne 0) { throw "Visual Studio toolchain initialization failed" }
    foreach ($variable in $variables) {
        if ($variable -match '^([^=]+)=(.*)$') {
            [Environment]::SetEnvironmentVariable($matches[1], $matches[2], "Process")
        }
    }
}
$bindings = @(if ($Binding -eq "All") { "Rust", "Node" } else { $Binding })
Push-Location $root
try {
    if (-not $SkipBuild) {
        if ($bindings -contains "Rust") { Invoke-Checked { cargo build --locked -p meeting-record-windows-e2e } }
        if ($bindings -contains "Node") { Invoke-Checked { npm run build --workspace meeting-record } }
    }
    Push-Location $build
    try {
        Invoke-Checked { cl.exe /nologo /std:c++17 /EHsc /W4 "$PSScriptRoot/windows-render-tone.cpp" /Fe:windows-render-tone.exe ole32.lib uuid.lib }
        if ($EndpointReference) {
            Invoke-Checked { cl.exe /nologo /std:c++17 /EHsc /W4 "$PSScriptRoot/windows-loopback-probe.cpp" /Fe:windows-loopback-probe.exe ole32.lib uuid.lib Mmdevapi.lib }
        }
    } finally { Pop-Location }
    if ($CompatibilityTarget) {
        Copy-Item -LiteralPath "$build/windows-render-tone.exe" -Destination "$build/ms-teams.exe" -Force
    }

    $console = & $devices -Output -Role Console
    $communications = & $devices -Output -Role Communications
    if (-not $console -or -not $communications) { throw "Both default output roles are required" }
    $outputs = @(& $devices -Output -Action List)
    if (-not $SeparateOutput) {
        $SeparateOutput = $outputs | Where-Object { $_ -ne $console } | Select-Object -First 1
    }
    if ($SeparateOutput) {
        $SeparateOutput = & $devices -Output -Action Find -Device $SeparateOutput
        if (-not $SeparateOutput -or $SeparateOutput -eq $console) { throw "SeparateOutput must name a different active output" }
    }
    Write-Host "console=$console; original communications=$communications; separate=$SeparateOutput"
    $saved = @{}
    Get-ChildItem Env:MREC_* | ForEach-Object { $saved[$_.Name] = $_.Value }
    Get-ChildItem Env:MREC_* | ForEach-Object { [Environment]::SetEnvironmentVariable($_.Name, [NullString]::Value, "Process") }
    $failures = @()
    try {
        if ($AllowSystemAudioFallback) { $env:MREC_ALLOW_SYSTEM_AUDIO_FALLBACK = "1" }
        if ($CompatibilityTarget -and $AllowSystemAudioFallback) { $env:MREC_EXPECT_SYSTEM_FALLBACK = "1" }
        $routes = @(@{ Name = "shared"; Device = $console })
        if ($SeparateOutput) { $routes += @{ Name = "separate"; Device = $SeparateOutput } }
        else { Write-Host "SKIP separate communications output: only one active output" }
        foreach ($route in $routes) {
            & $devices -Output -Role Communications -Device $route.Device | Out-Host
            foreach ($category in "ordinary", "communications") {
              # On shared outputs both roles resolve to the same endpoint; avoid duplicate cells.
              $roles = @(if ($route.Name -eq "separate") { "console", "communications" } else { "console" })
              foreach ($role in $roles) {
                $tones = @()
                try {
                    foreach ($frequency in 997, 440) {
                        $token = [Guid]::NewGuid().ToString("N")
                        $ready = Join-Path $build "$token.ready"
                        $toneArguments = @(
                            $role, $category, $frequency, 180, ('"' + $ready + '"')
                        )
                        if ($EndpointReference) { $toneArguments += "reference" }
                        $renderExecutable = if ($CompatibilityTarget -and $frequency -eq 997) { "$build/ms-teams.exe" } else { "$build/windows-render-tone.exe" }
                        $tone = Start-Process -WindowStyle Hidden -PassThru -FilePath $renderExecutable -ArgumentList $toneArguments -RedirectStandardOutput "$build/$token.stdout.log" -RedirectStandardError "$build/$token.stderr.log"
                        $tones += @{ Process = $tone; Ready = $ready; Log = "$build/$token.stdout.log" }
                        $deadline = [DateTime]::UtcNow.AddSeconds(10)
                        while (-not (Test-Path -LiteralPath $ready)) {
                            if ($tone.HasExited -or [DateTime]::UtcNow -gt $deadline) {
                                throw "render source not ready: $(Get-Content "$build/$token.stderr.log" -Raw)"
                            }
                            Start-Sleep -Milliseconds 50
                        }
                        Get-Content "$build/$token.stdout.log" | Out-Host
                    }
                    foreach ($bindingName in $bindings) {
                        foreach ($mode in "Process", "System") {
                            $name = "$($route.Name)/$role/$category/$bindingName/$mode"
                            Write-Host "==> $name"
                            [Environment]::SetEnvironmentVariable("MREC_DURATION_SECONDS", "$DurationSeconds", "Process")
                            [Environment]::SetEnvironmentVariable("MREC_MIX_SAMPLE_RATE", "16000", "Process")
                            if ($mode -eq "Process") {
                                [Environment]::SetEnvironmentVariable("MREC_TARGET_PID", "$($tones[0].Process.Id)", "Process")
                                if ($CompatibilityTarget -and $AllowSystemAudioFallback) {
                                    [Environment]::SetEnvironmentVariable("MREC_EXPECT_FREQUENCIES", "997,440", "Process")
                                    [Environment]::SetEnvironmentVariable("MREC_REJECT_FREQUENCIES", [NullString]::Value, "Process")
                                } else {
                                    [Environment]::SetEnvironmentVariable("MREC_EXPECT_FREQUENCIES", "997", "Process")
                                    [Environment]::SetEnvironmentVariable("MREC_REJECT_FREQUENCIES", "440", "Process")
                                }
                            } else {
                                [Environment]::SetEnvironmentVariable("MREC_TARGET_PID", [NullString]::Value, "Process")
                                [Environment]::SetEnvironmentVariable("MREC_EXPECT_FREQUENCIES", "997,440", "Process")
                                [Environment]::SetEnvironmentVariable("MREC_REJECT_FREQUENCIES", [NullString]::Value, "Process")
                            }
                            try {
                                if ($bindingName -eq "Rust") { Invoke-Checked { & "$root/target/debug/meeting-record-windows-e2e.exe" } }
                                else { Invoke-Checked { node "$PSScriptRoot/windows-node.mjs" } }
                                foreach ($tone in $tones) { if ($tone.Process.HasExited) { throw "render source exited during capture" } }
                                Write-Host "PASS $name"
                            } catch {
                                Write-Host "FAIL ${name}: $_"
                                $failures += $name
                            }
                        }
                    }
                    if ($EndpointReference) {
                        Invoke-Checked { & "$build/windows-loopback-probe.exe" include $tones[0].Process.Id 2 }
                        Invoke-Checked { & "$build/windows-loopback-probe.exe" exclude self 2 }
                    }
                } finally {
                    foreach ($tone in $tones) {
                        if (-not $tone.Process.HasExited) { Stop-Process -Id $tone.Process.Id -Force }
                        $tone.Process.WaitForExit()
                        if ($EndpointReference) {
                            Get-Content $tone.Log -Tail 2 | Out-Host
                            $lastReference = Get-Content $tone.Log -Tail 1
                            if ($lastReference -notmatch 'tone\d+_amplitude=([0-9.]+)' -or
                                [double]::Parse($matches[1], [Globalization.CultureInfo]::InvariantCulture) -lt 0.0001) {
                                $failures += "invalid endpoint reference: $($tone.Log)"
                            }
                        }
                        Remove-Item -LiteralPath $tone.Ready -Force -ErrorAction SilentlyContinue
                    }
                }
              }
            }
        }
    } finally {
        try { & $devices -Output -Role Communications -Device $communications | Out-Host }
        finally {
            Get-ChildItem Env:MREC_* | ForEach-Object { [Environment]::SetEnvironmentVariable($_.Name, [NullString]::Value, "Process") }
            foreach ($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name, $saved[$name], "Process") }
        }
    }
    if ($failures.Count) { throw "Failed: $($failures -join ', ')" }
    Write-Host "PASS: all available category/output/binding/capture-mode combinations"
} finally { Pop-Location }

[CmdletBinding()]
param(
    [ValidateSet("System", "Process", "All")]
    [string]$Mode = "All",
    [ValidateRange(2, 120)]
    [int]$DurationSeconds = 8,
    [ValidateRange(0.000001, 1.0)]
    [double]$MinimumPeak = 0.0001,
    [switch]$Microphone,
    [switch]$ExpectRecovery
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$harness = Join-Path $root "target\debug\meeting-record-windows-e2e.exe"
$toneScript = Join-Path $PSScriptRoot "windows-tone.ps1"
$variableNames = @(
    "MREC_TARGET_PID",
    "MREC_DURATION_SECONDS",
    "MREC_MIN_PEAK",
    "MREC_MICROPHONE",
    "MREC_EXPECT_RECOVERY"
)
$originalEnvironment = @{}
foreach ($name in $variableNames) {
    $originalEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, "Process")
}

Push-Location $root
try {
    & cargo build --locked -p meeting-record-windows-e2e
    if ($LASTEXITCODE -ne 0) {
        throw "failed to build the E2E capture harness"
    }

    $runCount = if ($Mode -eq "All") { 2 } else { 1 }
    $toneDuration = $DurationSeconds * $runCount + 20
    $powerShellExecutable = (Get-Process -Id $PID).Path
    $quotedToneScript = '"' + $toneScript + '"'
    $readyName = "meeting-record-tone-ready-$([Guid]::NewGuid().ToString('N'))"
    $readyFile = Join-Path ([IO.Path]::GetTempPath()) $readyName
    $quotedReadyFile = '"' + $readyFile + '"'
    $tone = Start-Process -PassThru -FilePath $powerShellExecutable -ArgumentList @(
        "-NoProfile",
        "-ExecutionPolicy", "Bypass",
        "-File", $quotedToneScript,
        "-DurationSeconds", $toneDuration,
        "-ReadyFile", $quotedReadyFile
    )
    try {
        $readyDeadline = [DateTime]::UtcNow.AddSeconds(20)
        while (-not (Test-Path -LiteralPath $readyFile)) {
            if ($tone.HasExited) {
                throw "the deterministic tone process exited before capture started"
            }
            if ([DateTime]::UtcNow -ge $readyDeadline) {
                throw "timed out waiting for the deterministic tone process"
            }
            Start-Sleep -Milliseconds 200
        }

        [Environment]::SetEnvironmentVariable(
            "MREC_DURATION_SECONDS", $DurationSeconds.ToString(), "Process")
        [Environment]::SetEnvironmentVariable(
            "MREC_MIN_PEAK",
            $MinimumPeak.ToString([Globalization.CultureInfo]::InvariantCulture),
            "Process")
        [Environment]::SetEnvironmentVariable(
            "MREC_MICROPHONE", $(if ($Microphone) { "1" } else { $null }), "Process")
        [Environment]::SetEnvironmentVariable(
            "MREC_EXPECT_RECOVERY", $(if ($ExpectRecovery) { "1" } else { $null }), "Process")

        if ($Mode -in @("System", "All")) {
            Write-Host "`n==> Windows system-audio E2E"
            [Environment]::SetEnvironmentVariable("MREC_TARGET_PID", $null, "Process")
            & $harness
            if ($LASTEXITCODE -ne 0) { throw "system-audio E2E failed" }
        }

        if ($Mode -in @("Process", "All")) {
            Write-Host "`n==> Windows process-audio E2E (pid $($tone.Id))"
            [Environment]::SetEnvironmentVariable(
                "MREC_TARGET_PID", $tone.Id.ToString(), "Process")
            & $harness
            if ($LASTEXITCODE -ne 0) { throw "process-audio E2E failed" }
        }
    }
    finally {
        if (-not $tone.HasExited) {
            Stop-Process -Id $tone.Id -Force -ErrorAction SilentlyContinue
            $tone.WaitForExit()
        }
        Remove-Item -LiteralPath $readyFile -Force -ErrorAction SilentlyContinue
    }
}
finally {
    foreach ($name in $variableNames) {
        [Environment]::SetEnvironmentVariable(
            $name, $originalEnvironment[$name], "Process")
    }
    Pop-Location
}

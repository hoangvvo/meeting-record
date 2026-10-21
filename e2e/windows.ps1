# Live-audio E2E for the Windows backend through the Rust and Node.js bindings.
#
# System and Process (the default "All") play their own tones. Meeting needs a call
# already running, such as a Google Meet tab receiving remote audio.
#
# -MicrophoneDevice plays a third tone into an output device that loops back to the
# default microphone, such as a virtual cable's "CABLE Input", and requires that tone
# on the microphone track and in the mixed output.
#
# -SwitchMicrophone makes another recording device the default mid-capture, then
# restores the original, and requires a microphone device change.
[CmdletBinding()]
param(
    [ValidateSet("System", "Process", "Meeting", "All")]
    [string]$Mode = "All",
    [string]$MeetingPlatform = "meet",
    [ValidateRange(0, [int]::MaxValue)]
    [int]$MeetingPid = 0,
    [ValidateSet("Rust", "Node", "All")]
    [string]$Binding = "All",
    [ValidateRange(2, 120)]
    [int]$DurationSeconds = 8,
    [ValidateRange(0.000001, 1.0)]
    [double]$MinimumPeak = 0.0001,
    [ValidateRange(0, 192000)]
    [int]$MixSampleRate = 16000,
    [ValidateRange(0, 10)]
    [double]$PauseSeconds = 1,
    [ValidateRange(0, 50)]
    [int]$RestartCycles = 2,
    [switch]$Microphone,
    [string]$MicrophoneDevice,
    [string]$SwitchMicrophone
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$rustHarness = Join-Path $root "target\debug\meeting-record-windows-e2e.exe"
$nodeHarness = Join-Path $PSScriptRoot "windows-node.mjs"
$toneScript = Join-Path $PSScriptRoot "windows-tone.ps1"
$microphoneScript = Join-Path $PSScriptRoot "windows-microphone.ps1"
# The captured process plays the first tone; the second must never reach it.
$targetFrequency = 997
$otherFrequency = 440
$microphoneFrequency = 660
$variableNames = @(
    "MREC_TARGET_PID",
    "MREC_DURATION_SECONDS",
    "MREC_MIN_PEAK",
    "MREC_MONO",
    "MREC_MICROPHONE",
    "MREC_EXPECT_DEVICE_CHANGE",
    "MREC_EXPECT_FREQUENCIES",
    "MREC_REJECT_FREQUENCIES",
    "MREC_MIX_SAMPLE_RATE",
    "MREC_PAUSE_SECONDS",
    "MREC_RESTART_CYCLES",
    "MREC_EXPECT_MEETING",
    "MREC_MICROPHONE_FREQUENCIES"
)
$originalEnvironment = @{}
foreach ($name in $variableNames) {
    $originalEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, "Process")
}

function Set-HarnessVariable([string]$Name, $Value) {
    [Environment]::SetEnvironmentVariable($Name, $(if ($null -eq $Value) { $null } else { "$Value" }), "Process")
}

# Windows PowerShell 5.1 turns redirected native stderr, such as cargo progress,
# into terminating errors under "Stop". Native commands report through exit codes.
function Invoke-Native([string]$Description, [scriptblock]$Command) {
    $ErrorActionPreference = "Continue"
    & $Command 2>&1 | ForEach-Object { "$_" }
    if ($LASTEXITCODE -ne 0) {
        throw "$Description failed with exit code $LASTEXITCODE"
    }
}

function Start-Tone([int]$Frequency, [int]$Seconds, [string]$Device) {
    $readyFile = Join-Path ([IO.Path]::GetTempPath()) "meeting-record-tone-ready-$([Guid]::NewGuid().ToString('N'))"
    $arguments = @(
        "-NoProfile",
        "-ExecutionPolicy", "Bypass",
        "-File", ('"' + $toneScript + '"'),
        "-DurationSeconds", $Seconds,
        "-Frequency", $Frequency,
        "-ReadyFile", ('"' + $readyFile + '"')
    )
    if ($Device) {
        $arguments += @("-Device", ('"' + $Device + '"'))
    }
    $process = Start-Process -PassThru -WindowStyle Hidden -FilePath (Get-Process -Id $PID).Path -ArgumentList $arguments
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    while (-not (Test-Path -LiteralPath $readyFile)) {
        if ($process.HasExited) {
            throw "the $Frequency Hz tone process exited before capture started"
        }
        if ([DateTime]::UtcNow -ge $deadline) {
            throw "timed out waiting for the $Frequency Hz tone process"
        }
        Start-Sleep -Milliseconds 200
    }
    [PSCustomObject]@{ Process = $process; ReadyFile = $readyFile }
}

function Stop-Tone($Tone) {
    if ($null -eq $Tone) { return }
    if (-not $Tone.Process.HasExited) {
        Stop-Process -Id $Tone.Process.Id -Force -ErrorAction SilentlyContinue
        $Tone.Process.WaitForExit()
    }
    Remove-Item -LiteralPath $Tone.ReadyFile -Force -ErrorAction SilentlyContinue
}

function Start-MicrophoneSwitch([string]$Device, [double]$DelaySeconds) {
    Start-Process -PassThru -WindowStyle Hidden -FilePath (Get-Process -Id $PID).Path -ArgumentList @(
        "-NoProfile",
        "-ExecutionPolicy", "Bypass",
        "-File", ('"' + $microphoneScript + '"'),
        "-Device", ('"' + $Device + '"'),
        "-DelaySeconds", $DelaySeconds.ToString([Globalization.CultureInfo]::InvariantCulture)
    )
}

Push-Location $root
$tones = @()
$originalMicrophone = $null
try {
    if ($SwitchMicrophone) {
        $originalMicrophone = & $microphoneScript
        if (-not $originalMicrophone) {
            throw "-SwitchMicrophone needs a default microphone to restore"
        }
    }

    $bindings = @(if ($Binding -eq "All") { "Rust", "Node" } else { $Binding })
    if ($bindings -contains "Rust") {
        Invoke-Native "building the Rust E2E harness" { cargo build --locked -p meeting-record-windows-e2e }
    }
    if ($bindings -contains "Node") {
        Invoke-Native "building the Node.js package" { npm run build --workspace meeting-record }
    }

    $modes = @(if ($Mode -eq "All") { "System", "Process" } else { $Mode })
    $runSeconds = ($DurationSeconds + 2) * $modes.Count * $bindings.Count + 30
    if ($Mode -ne "Meeting") {
        $target = Start-Tone $targetFrequency $runSeconds
        $tones += $target
        $tones += Start-Tone $otherFrequency $runSeconds
    }
    if ($MicrophoneDevice) {
        $tones += Start-Tone $microphoneFrequency $runSeconds $MicrophoneDevice
    }

    Set-HarnessVariable "MREC_DURATION_SECONDS" $DurationSeconds
    Set-HarnessVariable "MREC_MIN_PEAK" $MinimumPeak.ToString([Globalization.CultureInfo]::InvariantCulture)
    Set-HarnessVariable "MREC_MICROPHONE" $(if ($Microphone -or $MicrophoneDevice -or $SwitchMicrophone) { "1" })
    # A switched microphone carries another device's audio for a while.
    Set-HarnessVariable "MREC_MICROPHONE_FREQUENCIES" $(if ($MicrophoneDevice -and -not $SwitchMicrophone) { $microphoneFrequency })
    Set-HarnessVariable "MREC_EXPECT_DEVICE_CHANGE" $(if ($SwitchMicrophone) { "1" })
    Set-HarnessVariable "MREC_MIX_SAMPLE_RATE" $(if ($MixSampleRate -gt 0) { $MixSampleRate })
    # Pause cuts tracks up to a block apart; a switched microphone can be silent there.
    Set-HarnessVariable "MREC_PAUSE_SECONDS" $(if ($PauseSeconds -gt 0 -and -not $SwitchMicrophone) { $PauseSeconds.ToString([Globalization.CultureInfo]::InvariantCulture) })
    Set-HarnessVariable "MREC_RESTART_CYCLES" $RestartCycles

    foreach ($harness in $bindings) {
        foreach ($run in $modes) {
            if ($run -eq "Meeting") {
                # The detected meeting's app, with whatever audio the call carries.
                Write-Host "`n==> Windows $MeetingPlatform meeting E2E ($harness)"
                Set-HarnessVariable "MREC_EXPECT_MEETING" $MeetingPlatform
                Set-HarnessVariable "MREC_TARGET_PID" $(if ($MeetingPid -gt 0) { $MeetingPid })
                Set-HarnessVariable "MREC_MONO" $null
                Set-HarnessVariable "MREC_EXPECT_FREQUENCIES" $null
                Set-HarnessVariable "MREC_REJECT_FREQUENCIES" $null
            } elseif ($run -eq "System") {
                # Stereo system mix: both tones are present.
                Write-Host "`n==> Windows system-audio E2E ($harness)"
                Set-HarnessVariable "MREC_EXPECT_MEETING" $null
                Set-HarnessVariable "MREC_TARGET_PID" $null
                Set-HarnessVariable "MREC_MONO" $null
                Set-HarnessVariable "MREC_EXPECT_FREQUENCIES" "$targetFrequency,$otherFrequency"
                Set-HarnessVariable "MREC_REJECT_FREQUENCIES" $null
            } else {
                # Mono process tree: only the target's tone is present.
                Write-Host "`n==> Windows process-audio E2E ($harness, pid $($target.Process.Id))"
                Set-HarnessVariable "MREC_EXPECT_MEETING" $null
                Set-HarnessVariable "MREC_TARGET_PID" $target.Process.Id
                Set-HarnessVariable "MREC_MONO" "1"
                Set-HarnessVariable "MREC_EXPECT_FREQUENCIES" $targetFrequency
                Set-HarnessVariable "MREC_REJECT_FREQUENCIES" $(if ($MicrophoneDevice) { "$otherFrequency,$microphoneFrequency" } else { $otherFrequency })
            }
            $switches = @()
            if ($SwitchMicrophone) {
                # Switch a quarter of the way into capture, and back a quarter later.
                $switchAt = $RestartCycles * 0.4 + $DurationSeconds / 4
                $switches += Start-MicrophoneSwitch $SwitchMicrophone $switchAt
                $switches += Start-MicrophoneSwitch $originalMicrophone ($switchAt + $DurationSeconds / 4)
            }
            try {
                if ($harness -eq "Rust") {
                    Invoke-Native "$($run.ToLower())-audio E2E ($harness)" { & $rustHarness }
                } else {
                    Invoke-Native "$($run.ToLower())-audio E2E ($harness)" { node $nodeHarness }
                }
            }
            finally {
                foreach ($switch in $switches) { $switch.WaitForExit() }
            }
        }
    }
}
finally {
    foreach ($tone in $tones) { Stop-Tone $tone }
    if ($originalMicrophone) {
        & $microphoneScript -Device $originalMicrophone | Out-Null
    }
    foreach ($name in $variableNames) {
        [Environment]::SetEnvironmentVariable(
            $name, $originalEnvironment[$name], "Process")
    }
    Pop-Location
}

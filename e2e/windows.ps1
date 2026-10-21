# Live-audio E2E for the Windows backend through the Rust and Node.js bindings.
#
# Without -Mode, every scenario below runs in turn, each alone since two change the
# same microphone. Devices not named are detected: a VB-Audio cable for the
# microphone tone and removal, and other active devices for the switches. Scenarios
# without a device are skipped. -Mode runs one pass.
#
# System and Process play test tones. Meeting needs a call receiving audio. Exit
# ends the captured app mid-capture: only its system audio may stop, with app-exited.
# Helper captures a conferencing app, then starts another of its processes outside
# the captured tree, as Teams does with its call host; that audio must join.
#
# -MicrophoneDevice plays a third tone into an output device that loops back to the
# default microphone, such as a virtual cable's "CABLE Input", and requires that tone
# on the microphone track and in the mixed output.
#
# -SwitchMicrophone makes another recording device the default mid-capture, then
# restores the original, and requires a microphone device change.
#
# -RemoveMicrophone disables that recording device mid-capture, then restores it.
# The microphone track must resume, or move to another device when one exists,
# while system audio and the mix continue. Device notifications must bring it back
# promptly after the restore.
#
# -SwitchOutput makes another playback device the default mid-capture, then restores
# the original; capture must continue without gaps.
#
# Unless a scenario expects them, any track event fails the run.
[CmdletBinding()]
param(
    [ValidateSet("System", "Process", "Exit", "Helper", "Meeting", "All")]
    [string]$Mode,
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
    [string]$SwitchMicrophone,
    [string]$RemoveMicrophone,
    [string]$SwitchOutput
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$rustHarness = Join-Path $root "target\debug\meeting-record-windows-e2e.exe"
$nodeHarness = Join-Path $PSScriptRoot "windows-node.mjs"
$toneScript = Join-Path $PSScriptRoot "windows-tone.ps1"
$deviceScript = Join-Path $PSScriptRoot "windows-audio-device.ps1"
# The captured process plays the first tone; the second must never reach it.
$targetFrequency = 997
$otherFrequency = 440
$microphoneFrequency = 660
$excludedFrequency = 880
$helperFrequency = 523
# A copy of PowerShell named like a conferencing app, so the backend treats its
# processes as one app wherever they start.
$appDirectory = Join-Path ([IO.Path]::GetTempPath()) "meeting-record-e2e-app"
$appExecutable = Join-Path $appDirectory "Zoom.exe"
$restoredFile = Join-Path ([IO.Path]::GetTempPath()) "meeting-record-e2e-restored-$PID"
if (-not $Mode) {
    $shared = @{ Mode = "All" }
    foreach ($name in "Binding", "DurationSeconds", "MinimumPeak", "MixSampleRate", "PauseSeconds", "RestartCycles", "Microphone") {
        if ($PSBoundParameters.ContainsKey($name)) { $shared[$name] = $PSBoundParameters[$name] }
    }
    function Test-Device([string]$Device, [bool]$Output) {
        [bool]$Device -and [bool](& $deviceScript -Device $Device -Output:$Output -Action Find)
    }
    $recording = @(& $deviceScript -Action List)
    $playback = @(& $deviceScript -Output -Action List)
    # A VB-Audio cable plays what reaches "CABLE Input" into the "CABLE Output" microphone.
    $cable = @($playback -like "CABLE Input*").Count -gt 0 -and @($recording -like "CABLE Output*").Count -gt 0
    $useCable = $cable -and -not $PSBoundParameters.ContainsKey("MicrophoneDevice")
    if ($useCable) { $MicrophoneDevice = "CABLE Input" }
    if ($cable -and -not $PSBoundParameters.ContainsKey("RemoveMicrophone")) { $RemoveMicrophone = "CABLE Output" }
    if (-not $PSBoundParameters.ContainsKey("SwitchMicrophone")) {
        $SwitchMicrophone = $recording | Where-Object { $_ -notlike "CABLE *" } | Select-Object -First 1
    }
    if (-not $PSBoundParameters.ContainsKey("SwitchOutput")) {
        $defaultOutput = & $deviceScript -Output
        $SwitchOutput = $playback | Where-Object { $_ -ne $defaultOutput -and $_ -notlike "CABLE *" } | Select-Object -First 1
    }
    # The cable's tone reaches the microphone track only while it is the default input.
    $originalMicrophone = & $deviceScript
    if ($useCable) { & $deviceScript -Device "CABLE Output" | Out-Null }
    $loopback = @{}
    if (Test-Device $MicrophoneDevice $true) {
        $loopback.MicrophoneDevice = $MicrophoneDevice
    } elseif ($MicrophoneDevice) {
        Write-Host "no active playback device matches '$MicrophoneDevice'; running without the microphone tone"
    }
    $scenarios = @(
        @{ Name = "capture and app exit"; Arguments = $loopback },
        @{ Name = "microphone switch"; Device = $SwitchMicrophone; Output = $false; Arguments = @{ SwitchMicrophone = $SwitchMicrophone } },
        @{ Name = "microphone removal"; Device = $RemoveMicrophone; Output = $false; Arguments = @{ RemoveMicrophone = $RemoveMicrophone } + $loopback },
        @{ Name = "output switch"; Device = $SwitchOutput; Output = $true; Arguments = @{ SwitchOutput = $SwitchOutput } }
    )
    $failed = @()
    try {
        foreach ($scenario in $scenarios) {
            if ($scenario.ContainsKey("Device") -and -not (Test-Device $scenario.Device $scenario.Output)) {
                $reason = if ($scenario.Device) { "no active device matches '$($scenario.Device)'" } else { "no device found" }
                Write-Host "`n### skipped $($scenario.Name): $reason"
                continue
            }
            Write-Host "`n### $($scenario.Name)"
            $arguments = $scenario.Arguments
            try {
                & $PSCommandPath @shared @arguments
            } catch {
                Write-Host "FAILED $($scenario.Name): $_"
                $failed += $scenario.Name
            }
        }
    } finally {
        if ($useCable -and $originalMicrophone) { & $deviceScript -Device $originalMicrophone | Out-Null }
    }
    if ($failed) { throw "failed scenarios: $($failed -join ', ')" }
    Write-Host "`nPASS: every scenario that ran met its E2E assertions"
    return
}

$variableNames = @(
    "MREC_TARGET_PID",
    "MREC_DURATION_SECONDS",
    "MREC_MIN_PEAK",
    "MREC_MONO",
    "MREC_MICROPHONE",
    "MREC_EXPECT_DEVICE_CHANGE",
    "MREC_EXPECT_MICROPHONE_RECOVERY",
    "MREC_EXPECT_FREQUENCIES",
    "MREC_REJECT_FREQUENCIES",
    "MREC_MIX_SAMPLE_RATE",
    "MREC_PAUSE_SECONDS",
    "MREC_RESTART_CYCLES",
    "MREC_EXPECT_MEETING",
    "MREC_MICROPHONE_FREQUENCIES",
    "MREC_EXCLUDED_TONE_FREQUENCY",
    "MREC_TONE_POWERSHELL",
    "MREC_EXPECT_APP_EXIT",
    "MREC_HELPER_TONE_FREQUENCY",
    "MREC_HELPER_EXE",
    "MREC_MICROPHONE_RESTORED_FILE"
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

function Start-Tone([int]$Frequency, [int]$Seconds, [string]$Device, [string]$Executable) {
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
    $program = if ($Executable) { $Executable } else { (Get-Process -Id $PID).Path }
    $process = Start-Process -PassThru -WindowStyle Hidden -FilePath $program -ArgumentList $arguments
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

# Changes a device after a delay, from another process so capture keeps running.
function Start-DeviceChange([string]$Device, [double]$DelaySeconds, [string[]]$Options = @()) {
    Start-Process -PassThru -WindowStyle Hidden -FilePath (Get-Process -Id $PID).Path -ArgumentList (@(
        "-NoProfile",
        "-ExecutionPolicy", "Bypass",
        "-File", ('"' + $deviceScript + '"'),
        "-Device", ('"' + $Device + '"'),
        "-DelaySeconds", $DelaySeconds.ToString([Globalization.CultureInfo]::InvariantCulture)
    ) + $Options)
}

Push-Location $root
$tones = @()
$originalMicrophone = $null
$originalOutput = $null
try {
    if ($SwitchMicrophone) {
        $originalMicrophone = & $deviceScript
        if (-not $originalMicrophone) {
            throw "-SwitchMicrophone needs a default microphone to restore"
        }
    }
    if ($SwitchOutput) {
        $originalOutput = & $deviceScript -Output
        if (-not $originalOutput) {
            throw "-SwitchOutput needs a default playback device to restore"
        }
    }

    $bindings = @(if ($Binding -eq "All") { "Rust", "Node" } else { $Binding })
    if ($bindings -contains "Rust") {
        Invoke-Native "building the Rust E2E harness" { cargo build --locked -p meeting-record-windows-e2e }
    }
    if ($bindings -contains "Node") {
        Invoke-Native "building the Node.js package" { npm run build --workspace meeting-record }
    }

    $modes = @(if ($Mode -eq "All") { "System", "Process", "Exit", "Helper" } else { $Mode })
    if ($modes -contains "Helper") {
        New-Item -ItemType Directory -Force $appDirectory | Out-Null
        Copy-Item -LiteralPath "$env:WINDIR\System32\WindowsPowerShell\v1.0\powershell.exe" $appExecutable -Force
    }
    $runSeconds = ($DurationSeconds + 2) * $modes.Count * $bindings.Count + 30
    if ($modes -contains "System") { $runSeconds += 20 * $bindings.Count }
    if ($Mode -notin "Meeting", "Exit") {
        $target = Start-Tone $targetFrequency $runSeconds
        $tones += $target
        $tones += Start-Tone $otherFrequency $runSeconds
    }
    if ($MicrophoneDevice) {
        $tones += Start-Tone $microphoneFrequency $runSeconds $MicrophoneDevice
    }

    Set-HarnessVariable "MREC_DURATION_SECONDS" $DurationSeconds
    Set-HarnessVariable "MREC_MIN_PEAK" $MinimumPeak.ToString([Globalization.CultureInfo]::InvariantCulture)
    Set-HarnessVariable "MREC_MICROPHONE" $(if ($Microphone -or $MicrophoneDevice -or $SwitchMicrophone -or $RemoveMicrophone) { "1" })
    # A switched microphone carries another device's audio for a while, and a removed
    # one carries none.
    $microphoneChanges = $SwitchMicrophone -or $RemoveMicrophone
    Set-HarnessVariable "MREC_MICROPHONE_FREQUENCIES" $(if ($MicrophoneDevice -and -not $microphoneChanges) { $microphoneFrequency })
    Set-HarnessVariable "MREC_EXPECT_DEVICE_CHANGE" $(if ($SwitchMicrophone) { "1" })
    Set-HarnessVariable "MREC_EXPECT_MICROPHONE_RECOVERY" $(if ($RemoveMicrophone) { "1" })
    Set-HarnessVariable "MREC_MIX_SAMPLE_RATE" $(if ($MixSampleRate -gt 0) { $MixSampleRate })
    # Pause cuts tracks up to a block apart; a changed microphone can be silent there.
    Set-HarnessVariable "MREC_PAUSE_SECONDS" $(if ($PauseSeconds -gt 0 -and -not $microphoneChanges) { $PauseSeconds.ToString([Globalization.CultureInfo]::InvariantCulture) })
    Set-HarnessVariable "MREC_RESTART_CYCLES" $RestartCycles
    Set-HarnessVariable "MREC_TONE_POWERSHELL" (Get-Process -Id $PID).Path

    foreach ($harness in $bindings) {
        foreach ($run in $modes) {
            Set-HarnessVariable "MREC_EXCLUDED_TONE_FREQUENCY" $null
            Set-HarnessVariable "MREC_EXPECT_APP_EXIT" $null
            Set-HarnessVariable "MREC_HELPER_TONE_FREQUENCY" $null
            Set-HarnessVariable "MREC_HELPER_EXE" $null
            if ($run -eq "Meeting") {
                # The detected meeting's app, with whatever audio the call carries.
                Write-Host "`n==> Windows $MeetingPlatform meeting E2E ($harness)"
                Set-HarnessVariable "MREC_EXPECT_MEETING" $MeetingPlatform
                Set-HarnessVariable "MREC_TARGET_PID" $(if ($MeetingPid -gt 0) { $MeetingPid })
                Set-HarnessVariable "MREC_MONO" $null
                Set-HarnessVariable "MREC_EXPECT_FREQUENCIES" $null
                Set-HarnessVariable "MREC_REJECT_FREQUENCIES" $null
            } elseif ($run -eq "System") {
                # Sibling tones remain; a recorder child launched after start is excluded.
                Write-Host "`n==> Windows system-audio E2E ($harness)"
                Set-HarnessVariable "MREC_EXPECT_MEETING" $null
                Set-HarnessVariable "MREC_TARGET_PID" $null
                Set-HarnessVariable "MREC_MONO" $null
                Set-HarnessVariable "MREC_EXPECT_FREQUENCIES" "$targetFrequency,$otherFrequency"
                $checkExclusion = -not ($microphoneChanges -or $SwitchOutput)
                Set-HarnessVariable "MREC_REJECT_FREQUENCIES" $(if ($checkExclusion) { $excludedFrequency })
                Set-HarnessVariable "MREC_EXCLUDED_TONE_FREQUENCY" $(if ($checkExclusion) { $excludedFrequency })
            } elseif ($run -eq "Exit") {
                # The harness ends this app, so it gets its own tone.
                $exitTone = Start-Tone $targetFrequency 30
                $tones += $exitTone
                Write-Host "`n==> Windows app-exit E2E ($harness, pid $($exitTone.Process.Id))"
                Set-HarnessVariable "MREC_EXPECT_MEETING" $null
                Set-HarnessVariable "MREC_TARGET_PID" $exitTone.Process.Id
                Set-HarnessVariable "MREC_MONO" "1"
                Set-HarnessVariable "MREC_EXPECT_FREQUENCIES" $null
                Set-HarnessVariable "MREC_REJECT_FREQUENCIES" $null
                Set-HarnessVariable "MREC_EXPECT_APP_EXIT" "1"
            } elseif ($run -eq "Helper") {
                # The harness starts a second copy of the app after capture begins.
                $appTone = Start-Tone $targetFrequency ($DurationSeconds + 30) -Executable $appExecutable
                $tones += $appTone
                Write-Host "`n==> Windows app-helper E2E ($harness, pid $($appTone.Process.Id))"
                Set-HarnessVariable "MREC_EXPECT_MEETING" $null
                Set-HarnessVariable "MREC_TARGET_PID" $appTone.Process.Id
                Set-HarnessVariable "MREC_MONO" "1"
                Set-HarnessVariable "MREC_EXPECT_FREQUENCIES" "$targetFrequency,$helperFrequency"
                Set-HarnessVariable "MREC_REJECT_FREQUENCIES" $(if ($MicrophoneDevice) { "$otherFrequency,$microphoneFrequency" } else { $otherFrequency })
                Set-HarnessVariable "MREC_HELPER_TONE_FREQUENCY" $helperFrequency
                Set-HarnessVariable "MREC_HELPER_EXE" $appExecutable
            } else {
                # Mono process tree: only the target's tone is present.
                Write-Host "`n==> Windows process-audio E2E ($harness, pid $($target.Process.Id))"
                Set-HarnessVariable "MREC_EXPECT_MEETING" $null
                Set-HarnessVariable "MREC_TARGET_PID" $target.Process.Id
                Set-HarnessVariable "MREC_MONO" "1"
                Set-HarnessVariable "MREC_EXPECT_FREQUENCIES" $targetFrequency
                Set-HarnessVariable "MREC_REJECT_FREQUENCIES" $(if ($MicrophoneDevice) { "$otherFrequency,$microphoneFrequency" } else { $otherFrequency })
            }
            $changes = @()
            Remove-Item -LiteralPath $restoredFile -Force -ErrorAction SilentlyContinue
            Set-HarnessVariable "MREC_MICROPHONE_RESTORED_FILE" $(if ($RemoveMicrophone -and $run -ne "Exit") { $restoredFile })
            if ($run -ne "Exit") {
                # Change a quarter of the way into capture, and back a quarter later.
                $changeAt = $RestartCycles * 0.4 + $DurationSeconds / 4
                $restoreAt = $changeAt + $DurationSeconds / 4
                if ($SwitchMicrophone) {
                    $changes += Start-DeviceChange $SwitchMicrophone $changeAt
                    $changes += Start-DeviceChange $originalMicrophone $restoreAt
                }
                if ($RemoveMicrophone) {
                    $changes += Start-DeviceChange $RemoveMicrophone $changeAt @("-Action", "Disable")
                    # Off the 500 ms retry grid: only a device notification brings it back at once.
                    $changes += Start-DeviceChange $RemoveMicrophone ($restoreAt + 0.25) @("-Action", "Enable", "-DoneFile", ('"' + $restoredFile + '"'))
                }
                # A helper starting mid-switch opens on whichever output is default then.
                if ($SwitchOutput -and $run -ne "Helper") {
                    $changes += Start-DeviceChange $SwitchOutput $changeAt @("-Output")
                    $changes += Start-DeviceChange $originalOutput $restoreAt @("-Output")
                }
            }
            try {
                if ($harness -eq "Rust") {
                    Invoke-Native "$($run.ToLower())-audio E2E ($harness)" { & $rustHarness }
                } else {
                    Invoke-Native "$($run.ToLower())-audio E2E ($harness)" { node $nodeHarness }
                }
            }
            finally {
                foreach ($change in $changes) { $change.WaitForExit() }
                # Another run's copy of the app would join as a sibling.
                if ($run -eq "Helper") { Stop-Tone $appTone }
            }
        }
    }
}
finally {
    foreach ($tone in $tones) { Stop-Tone $tone }
    Remove-Item -LiteralPath $restoredFile -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $appDirectory -Recurse -Force -ErrorAction SilentlyContinue
    if ($RemoveMicrophone) {
        & $deviceScript -Device $RemoveMicrophone -Action Enable | Out-Null
    }
    if ($originalMicrophone) {
        & $deviceScript -Device $originalMicrophone | Out-Null
    }
    if ($originalOutput) {
        & $deviceScript -Output -Device $originalOutput | Out-Null
    }
    foreach ($name in $variableNames) {
        [Environment]::SetEnvironmentVariable(
            $name, $originalEnvironment[$name], "Process")
    }
    Pop-Location
}

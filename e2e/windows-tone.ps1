[CmdletBinding()]
param(
    [ValidateRange(1, 600)]
    [int]$DurationSeconds = 30,
    [ValidateRange(100, 5000)]
    [int]$Frequency = 997,
    [ValidateRange(0.01, 0.95)]
    [double]$Amplitude = 0.2,
    [string]$ReadyFile
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$sampleRate = 48000
$channels = 2
$bitsPerSample = 16
$frameCount = $sampleRate
$blockAlign = $channels * ($bitsPerSample / 8)
$dataSize = $frameCount * $blockAlign
$tonePath = Join-Path ([IO.Path]::GetTempPath()) "meeting-record-tone-$PID.wav"

$stream = [IO.File]::Open($tonePath, [IO.FileMode]::Create,
    [IO.FileAccess]::Write, [IO.FileShare]::Read)
$writer = [IO.BinaryWriter]::new($stream)
try {
    $writer.Write([Text.Encoding]::ASCII.GetBytes("RIFF"))
    $writer.Write([int](36 + $dataSize))
    $writer.Write([Text.Encoding]::ASCII.GetBytes("WAVEfmt "))
    $writer.Write([int]16)
    $writer.Write([int16]1)
    $writer.Write([int16]$channels)
    $writer.Write([int]$sampleRate)
    $writer.Write([int]($sampleRate * $blockAlign))
    $writer.Write([int16]$blockAlign)
    $writer.Write([int16]$bitsPerSample)
    $writer.Write([Text.Encoding]::ASCII.GetBytes("data"))
    $writer.Write([int]$dataSize)
    for ($frame = 0; $frame -lt $frameCount; $frame++) {
        $phase = 2.0 * [Math]::PI * $Frequency * $frame / $sampleRate
        $sample = [int16][Math]::Round([Math]::Sin($phase) * [int16]::MaxValue * $Amplitude)
        for ($channel = 0; $channel -lt $channels; $channel++) {
            $writer.Write($sample)
        }
    }
}
finally {
    $writer.Dispose()
}

try {
    if (-not ("MeetingRecord.NativeAudio" -as [type])) {
        Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
namespace MeetingRecord {
    public static class NativeAudio {
        [DllImport("winmm.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        public static extern bool PlaySound(string sound, IntPtr module, uint flags);
    }
}
"@
    }

    $sndAsync = 0x0001
    $sndNodefault = 0x0002
    $sndLoop = 0x0008
    $sndFilename = 0x00020000
    $started = [MeetingRecord.NativeAudio]::PlaySound(
        $tonePath, [IntPtr]::Zero, $sndAsync -bor $sndNodefault -bor $sndLoop -bor $sndFilename)
    if (-not $started) {
        throw "winmm PlaySound failed with error $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
    }
    if ($ReadyFile) {
        [IO.File]::WriteAllText($ReadyFile, $PID.ToString())
    }
    Write-Host "READY: tone pid=$PID frequency=${Frequency}Hz"
    Start-Sleep -Seconds $DurationSeconds
}
finally {
    if ("MeetingRecord.NativeAudio" -as [type]) {
        [void][MeetingRecord.NativeAudio]::PlaySound($null, [IntPtr]::Zero, 0)
    }
    if ($ReadyFile) {
        Remove-Item -LiteralPath $ReadyFile -Force -ErrorAction SilentlyContinue
    }
    Remove-Item -LiteralPath $tonePath -Force -ErrorAction SilentlyContinue
}

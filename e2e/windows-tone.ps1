[CmdletBinding()]
param(
    [ValidateRange(1, 600)]
    [int]$DurationSeconds = 30,
    [ValidateRange(100, 5000)]
    [int]$Frequency = 997,
    [ValidateRange(0.01, 0.95)]
    [double]$Amplitude = 0.2,
    [string]$ReadyFile,
    # Play through the output device whose name contains this text instead of the
    # default device, such as a virtual cable that feeds a test microphone.
    [string]$Device
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

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        struct WaveOutCaps {
            public ushort Manufacturer;
            public ushort Product;
            public uint DriverVersion;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string Name;
            public uint Formats;
            public ushort Channels;
            public ushort Reserved;
            public uint Support;
        }

        [StructLayout(LayoutKind.Sequential)]
        struct WaveFormat {
            public ushort FormatTag;
            public ushort Channels;
            public uint SamplesPerSec;
            public uint AvgBytesPerSec;
            public ushort BlockAlign;
            public ushort BitsPerSample;
            public ushort Size;
        }

        [StructLayout(LayoutKind.Sequential)]
        struct WaveHeader {
            public IntPtr Data;
            public uint BufferLength;
            public uint BytesRecorded;
            public IntPtr User;
            public uint Flags;
            public uint Loops;
            public IntPtr Next;
            public IntPtr Reserved;
        }

        const uint BeginLoop = 0x4;
        const uint EndLoop = 0x8;

        [DllImport("winmm.dll")] static extern uint waveOutGetNumDevs();
        [DllImport("winmm.dll", CharSet = CharSet.Unicode)]
        static extern uint waveOutGetDevCaps(UIntPtr device, ref WaveOutCaps caps, uint size);
        [DllImport("winmm.dll")]
        static extern uint waveOutOpen(out IntPtr output, UIntPtr device, ref WaveFormat format,
            IntPtr callback, IntPtr instance, uint flags);
        [DllImport("winmm.dll")] static extern uint waveOutPrepareHeader(IntPtr output, IntPtr header, uint size);
        [DllImport("winmm.dll")] static extern uint waveOutUnprepareHeader(IntPtr output, IntPtr header, uint size);
        [DllImport("winmm.dll")] static extern uint waveOutWrite(IntPtr output, IntPtr header, uint size);
        [DllImport("winmm.dll")] static extern uint waveOutReset(IntPtr output);
        [DllImport("winmm.dll")] static extern uint waveOutClose(IntPtr output);

        static IntPtr output = IntPtr.Zero;
        static IntPtr header = IntPtr.Zero;
        static IntPtr data = IntPtr.Zero;

        // Loops a 16-bit PCM WAV with a 44-byte header on the first output device
        // whose name contains `device`, and returns that device's name.
        public static string Loop(string device, string wavPath, int sampleRate, int channels) {
            byte[] wav = System.IO.File.ReadAllBytes(wavPath);
            uint count = waveOutGetNumDevs();
            for (uint index = 0; index < count; index++) {
                WaveOutCaps caps = new WaveOutCaps();
                if (waveOutGetDevCaps((UIntPtr)index, ref caps, (uint)Marshal.SizeOf(caps)) != 0 ||
                    caps.Name.IndexOf(device, StringComparison.OrdinalIgnoreCase) < 0) {
                    continue;
                }
                WaveFormat format = new WaveFormat {
                    FormatTag = 1,
                    Channels = (ushort)channels,
                    SamplesPerSec = (uint)sampleRate,
                    AvgBytesPerSec = (uint)(sampleRate * channels * 2),
                    BlockAlign = (ushort)(channels * 2),
                    BitsPerSample = 16
                };
                uint result = waveOutOpen(out output, (UIntPtr)index, ref format, IntPtr.Zero, IntPtr.Zero, 0);
                if (result != 0) throw new InvalidOperationException("waveOutOpen failed with " + result);
                data = Marshal.AllocHGlobal(wav.Length - 44);
                Marshal.Copy(wav, 44, data, wav.Length - 44);
                uint size = (uint)Marshal.SizeOf(typeof(WaveHeader));
                header = Marshal.AllocHGlobal((int)size);
                Marshal.StructureToPtr(new WaveHeader { Data = data, BufferLength = (uint)(wav.Length - 44) }, header, false);
                waveOutPrepareHeader(output, header, size);
                WaveHeader prepared = (WaveHeader)Marshal.PtrToStructure(header, typeof(WaveHeader));
                prepared.Flags |= BeginLoop | EndLoop;
                prepared.Loops = uint.MaxValue;
                Marshal.StructureToPtr(prepared, header, false);
                result = waveOutWrite(output, header, size);
                if (result != 0) throw new InvalidOperationException("waveOutWrite failed with " + result);
                return caps.Name;
            }
            throw new ArgumentException("no output device name contains " + device);
        }

        public static void Stop() {
            if (output == IntPtr.Zero) return;
            waveOutReset(output);
            waveOutUnprepareHeader(output, header, (uint)Marshal.SizeOf(typeof(WaveHeader)));
            waveOutClose(output);
            Marshal.FreeHGlobal(header);
            Marshal.FreeHGlobal(data);
            output = IntPtr.Zero;
        }
    }
}
"@
    }

    if ($Device) {
        # PlaySound can only reach the default output device.
        $deviceName = [MeetingRecord.NativeAudio]::Loop($Device, $tonePath, $sampleRate, $channels)
    } else {
        $sndAsync = 0x0001
        $sndNodefault = 0x0002
        $sndLoop = 0x0008
        $sndFilename = 0x00020000
        $started = [MeetingRecord.NativeAudio]::PlaySound(
            $tonePath, [IntPtr]::Zero, $sndAsync -bor $sndNodefault -bor $sndLoop -bor $sndFilename)
        if (-not $started) {
            throw "winmm PlaySound failed with error $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
        }
        $deviceName = "default output"
    }
    if ($ReadyFile) {
        [IO.File]::WriteAllText($ReadyFile, $PID.ToString())
    }
    Write-Host "READY: tone pid=$PID frequency=${Frequency}Hz device=$deviceName"
    Start-Sleep -Seconds $DurationSeconds
}
finally {
    if ("MeetingRecord.NativeAudio" -as [type]) {
        [MeetingRecord.NativeAudio]::Stop()
        [void][MeetingRecord.NativeAudio]::PlaySound($null, [IntPtr]::Zero, 0)
    }
    if ($ReadyFile) {
        Remove-Item -LiteralPath $ReadyFile -Force -ErrorAction SilentlyContinue
    }
    Remove-Item -LiteralPath $tonePath -Force -ErrorAction SilentlyContinue
}

[CmdletBinding()]
param(
    # Act on the recording device whose name contains this text, or with -Output, the
    # playback device. Without it, print the current default's name.
    [string]$Device,
    [switch]$Output,
    # Default makes the device the default for every role. Disable removes it, as
    # unplugging would; Enable restores it and makes it the default again. Find
    # prints the active device's full name, or nothing. List prints every active
    # device's name.
    [ValidateSet("Default", "Disable", "Enable", "Find", "List")]
    [string]$Action = "Default",
    [ValidateRange(0, 600)]
    [double]$DelaySeconds = 0
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if (-not ("MeetingRecord.AudioDevices" -as [type])) {
    # IPolicyConfig is undocumented; it is how Windows' own settings switch devices.
    Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
using System.Threading;
namespace MeetingRecord {
    [ComImport, Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IMMDeviceEnumerator {
        [PreserveSig] int EnumAudioEndpoints(int flow, int stateMask, out IMMDeviceCollection devices);
        [PreserveSig] int GetDefaultAudioEndpoint(int flow, int role, out IMMDevice device);
    }
    [ComImport, Guid("0BD7A1BE-7A1A-44DB-8397-CC5392387B5E"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IMMDeviceCollection {
        [PreserveSig] int GetCount(out int count);
        [PreserveSig] int Item(int index, out IMMDevice device);
    }
    [ComImport, Guid("D666063F-1587-4E43-81F1-B948E807363F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IMMDevice {
        [PreserveSig] int Activate(ref Guid iid, int context, IntPtr parameters, out IntPtr result);
        [PreserveSig] int OpenPropertyStore(int access, out IPropertyStore store);
        [PreserveSig] int GetId([MarshalAs(UnmanagedType.LPWStr)] out string id);
    }
    [ComImport, Guid("886d8eeb-8cf2-4446-8d02-cdba1dbdcf99"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IPropertyStore {
        [PreserveSig] int GetCount(out int count);
        [PreserveSig] int GetAt(int index, out PropertyKey key);
        [PreserveSig] int GetValue(ref PropertyKey key, out PropVariant value);
    }
    [StructLayout(LayoutKind.Sequential)] struct PropertyKey { public Guid Format; public int Id; }
    [StructLayout(LayoutKind.Sequential)] struct PropVariant { public short Type; short A, B, C; public IntPtr Pointer; IntPtr D; }
    [ComImport, Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")] class MMDeviceEnumerator {}
    [ComImport, Guid("f8679f50-850a-41cf-9c72-430f290290c8"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IPolicyConfig {
        [PreserveSig] int GetMixFormat([MarshalAs(UnmanagedType.LPWStr)] string device, IntPtr format);
        [PreserveSig] int GetDeviceFormat([MarshalAs(UnmanagedType.LPWStr)] string device, int useDefault, IntPtr format);
        [PreserveSig] int ResetDeviceFormat([MarshalAs(UnmanagedType.LPWStr)] string device);
        [PreserveSig] int SetDeviceFormat([MarshalAs(UnmanagedType.LPWStr)] string device, IntPtr endpointFormat, IntPtr mixFormat);
        [PreserveSig] int GetProcessingPeriod([MarshalAs(UnmanagedType.LPWStr)] string device, int useDefault, IntPtr defaultPeriod, IntPtr minimumPeriod);
        [PreserveSig] int SetProcessingPeriod([MarshalAs(UnmanagedType.LPWStr)] string device, IntPtr period);
        [PreserveSig] int GetShareMode([MarshalAs(UnmanagedType.LPWStr)] string device, IntPtr mode);
        [PreserveSig] int SetShareMode([MarshalAs(UnmanagedType.LPWStr)] string device, IntPtr mode);
        [PreserveSig] int GetPropertyValue([MarshalAs(UnmanagedType.LPWStr)] string device, int store, IntPtr key, IntPtr value);
        [PreserveSig] int SetPropertyValue([MarshalAs(UnmanagedType.LPWStr)] string device, int store, IntPtr key, IntPtr value);
        [PreserveSig] int SetDefaultEndpoint([MarshalAs(UnmanagedType.LPWStr)] string device, int role);
        [PreserveSig] int SetEndpointVisibility([MarshalAs(UnmanagedType.LPWStr)] string device, int visible);
    }
    [ComImport, Guid("870af99c-171d-4f9e-af0d-e63df40c2bc9")] class PolicyConfigClient {}

    public static class AudioDevices {
        public const int Playback = 0;
        public const int Recording = 1;
        const int Active = 1;
        const int Disabled = 2;

        static string Name(IMMDevice device) {
            IPropertyStore store;
            if (device.OpenPropertyStore(0, out store) != 0) return "";
            PropertyKey key = new PropertyKey { Format = new Guid("a45c254e-df1c-4efd-8020-67d146a850e0"), Id = 14 };
            PropVariant value;
            if (store.GetValue(ref key, out value) != 0 || value.Type != 31) return "";
            return Marshal.PtrToStringUni(value.Pointer);
        }

        public static string Default(int flow) {
            IMMDevice device;
            int result = ((IMMDeviceEnumerator)new MMDeviceEnumerator()).GetDefaultAudioEndpoint(flow, 0, out device);
            return result == 0 ? Name(device) : "";
        }

        // The id and full name of the first device in `states` whose name contains `text`.
        static string Find(int flow, int states, string text, out string name) {
            IMMDeviceCollection devices;
            ((IMMDeviceEnumerator)new MMDeviceEnumerator()).EnumAudioEndpoints(flow, states, out devices);
            int count;
            devices.GetCount(out count);
            for (int index = 0; index < count; index++) {
                IMMDevice device;
                devices.Item(index, out device);
                name = Name(device);
                if (name.IndexOf(text, StringComparison.OrdinalIgnoreCase) < 0) continue;
                string id;
                device.GetId(out id);
                return id;
            }
            throw new ArgumentException("no matching " + (flow == Playback ? "playback" : "recording") +
                                        " device name contains " + text);
        }

        public static string[] ListActive(int flow) {
            IMMDeviceCollection devices;
            ((IMMDeviceEnumerator)new MMDeviceEnumerator()).EnumAudioEndpoints(flow, Active, out devices);
            int count;
            devices.GetCount(out count);
            string[] names = new string[count];
            for (int index = 0; index < count; index++) {
                IMMDevice device;
                devices.Item(index, out device);
                names[index] = Name(device);
            }
            return names;
        }

        public static string FindActive(int flow, string text) {
            try {
                string name;
                Find(flow, Active, text, out name);
                return name;
            } catch (ArgumentException) {
                return "";
            }
        }

        static void MakeDefault(string id) {
            IPolicyConfig policy = (IPolicyConfig)new PolicyConfigClient();
            for (int role = 0; role < 3; role++) {
                int result = policy.SetDefaultEndpoint(id, role);
                if (result != 0) throw new InvalidOperationException("SetDefaultEndpoint failed with " + result);
            }
        }

        // Each returns the full name of the device changed.
        public static string SetDefault(int flow, string text) {
            string name;
            MakeDefault(Find(flow, Active, text, out name));
            return name;
        }

        public static string Disable(int flow, string text) {
            string name;
            int result = ((IPolicyConfig)new PolicyConfigClient()).SetEndpointVisibility(Find(flow, Active, text, out name), 0);
            if (result != 0) throw new InvalidOperationException("SetEndpointVisibility failed with " + result);
            return name;
        }

        public static string Enable(int flow, string text) {
            string name;
            string id = Find(flow, Active | Disabled, text, out name);
            int result = ((IPolicyConfig)new PolicyConfigClient()).SetEndpointVisibility(id, 1);
            if (result != 0) throw new InvalidOperationException("SetEndpointVisibility failed with " + result);
            // A re-enabled device takes a moment to become available as a default.
            for (int attempt = 0; ; attempt++) {
                try {
                    MakeDefault(id);
                    return name;
                } catch (InvalidOperationException) {
                    if (attempt == 20) throw;
                    Thread.Sleep(100);
                }
            }
        }
    }
}
"@
}

$flow = if ($Output) { [MeetingRecord.AudioDevices]::Playback } else { [MeetingRecord.AudioDevices]::Recording }
if ($Action -eq "List") {
    [MeetingRecord.AudioDevices]::ListActive($flow)
    return
}
if (-not $Device) {
    [MeetingRecord.AudioDevices]::Default($flow)
    return
}
if ($Action -eq "Find") {
    [MeetingRecord.AudioDevices]::FindActive($flow, $Device)
    return
}
Start-Sleep -Milliseconds ([int]($DelaySeconds * 1000))
$name = switch ($Action) {
    "Default" { [MeetingRecord.AudioDevices]::SetDefault($flow, $Device) }
    "Disable" { [MeetingRecord.AudioDevices]::Disable($flow, $Device) }
    "Enable" { [MeetingRecord.AudioDevices]::Enable($flow, $Device) }
}
Write-Host "$($Action.ToLower()) $(if ($Output) { 'output' } else { 'microphone' }): $name"

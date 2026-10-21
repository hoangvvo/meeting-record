[CmdletBinding()]
param(
    # Make the active recording device whose name contains this text the default
    # for every role. Without it, print the current default's name.
    [string]$Device,
    [ValidateRange(0, 600)]
    [double]$DelaySeconds = 0
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if (-not ("MeetingRecord.Microphones" -as [type])) {
    # IPolicyConfig is undocumented; it is how Windows' own settings switch devices.
    Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
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
    }
    [ComImport, Guid("870af99c-171d-4f9e-af0d-e63df40c2bc9")] class PolicyConfigClient {}

    public static class Microphones {
        const int Capture = 1;
        const int Active = 1;

        static string Name(IMMDevice device) {
            IPropertyStore store;
            if (device.OpenPropertyStore(0, out store) != 0) return "";
            PropertyKey key = new PropertyKey { Format = new Guid("a45c254e-df1c-4efd-8020-67d146a850e0"), Id = 14 };
            PropVariant value;
            if (store.GetValue(ref key, out value) != 0 || value.Type != 31) return "";
            return Marshal.PtrToStringUni(value.Pointer);
        }

        public static string Default() {
            IMMDevice device;
            int result = ((IMMDeviceEnumerator)new MMDeviceEnumerator()).GetDefaultAudioEndpoint(Capture, 0, out device);
            return result == 0 ? Name(device) : "";
        }

        // Returns the full name of the device made default.
        public static string SetDefault(string text) {
            IMMDeviceCollection devices;
            ((IMMDeviceEnumerator)new MMDeviceEnumerator()).EnumAudioEndpoints(Capture, Active, out devices);
            int count;
            devices.GetCount(out count);
            for (int index = 0; index < count; index++) {
                IMMDevice device;
                devices.Item(index, out device);
                string name = Name(device);
                if (name.IndexOf(text, StringComparison.OrdinalIgnoreCase) < 0) continue;
                string id;
                device.GetId(out id);
                IPolicyConfig policy = (IPolicyConfig)new PolicyConfigClient();
                for (int role = 0; role < 3; role++) {
                    int result = policy.SetDefaultEndpoint(id, role);
                    if (result != 0) throw new InvalidOperationException("SetDefaultEndpoint failed with " + result);
                }
                return name;
            }
            throw new ArgumentException("no active recording device name contains " + text);
        }
    }
}
"@
}

if (-not $Device) {
    [MeetingRecord.Microphones]::Default()
    return
}
Start-Sleep -Milliseconds ([int]($DelaySeconds * 1000))
$name = [MeetingRecord.Microphones]::SetDefault($Device)
Write-Host "default microphone: $name"

import CoreAudio
import Foundation

struct MicrophoneDevice: Equatable {
    var id: AudioObjectID
    var name: String
}

/// Resolve routing separately from the audio engine so selection is testable.
struct MicrophoneDeviceResolver {
    var defaultInput: () -> AudioObjectID
    var appInputs: ([pid_t]) -> [AudioObjectID]
    /// nil means a non-aggregate device; an empty array means no active members.
    var subdevices: (AudioObjectID) -> [AudioObjectID]?
    var hasInput: (AudioObjectID) -> Bool
    var name: (AudioObjectID) -> String

    func resolve(source: Int32, pids: [pid_t]) -> MicrophoneDevice? {
        let fallback = defaultInput()
        if source == 2 && !pids.isEmpty {
            let inputs = appInputs(pids).reduce(into: Set<AudioObjectID>()) {
                $0.formUnion(inputLeaves($1, visited: []))
            }
            // A single microphone track cannot follow conflicting app devices.
            if inputs.count == 1, let device = inputs.first {
                return MicrophoneDevice(id: device, name: name(device))
            }
        }
        guard fallback != kAudioObjectUnknown, hasInput(fallback) else { return nil }
        return MicrophoneDevice(id: fallback, name: name(fallback))
    }

    private func inputLeaves(_ device: AudioObjectID, visited: Set<AudioObjectID>) -> Set<AudioObjectID> {
        guard device != kAudioObjectUnknown, visited.count < 16,
              !visited.contains(device) else { return [] }
        guard let children = subdevices(device) else { return hasInput(device) ? [device] : [] }
        var path = visited
        path.insert(device)
        return children.reduce(into: Set<AudioObjectID>()) {
            $0.formUnion(inputLeaves($1, visited: path))
        }
    }

    static var system: MicrophoneDeviceResolver {
        MicrophoneDeviceResolver(
            defaultInput: {
                HAL.value(HAL.system, kAudioHardwarePropertyDefaultInputDevice,
                          default: AudioObjectID(kAudioObjectUnknown))
            },
            appInputs: { pids in
                let processes = AudioProcessRegistry.all()
                let apps = AudioAppRegistry.applications().filter(\.isRegular)
                let tree = ProcessTree.read(ancestorsOf: processes.map(\.pid))
                let targets = Set(AudioAppRegistry.includingHelpers(
                    pids, processes: processes, apps: apps, tree: tree))
                return processes.filter { targets.contains($0.pid) && $0.isRunningInput }
                    .flatMap { HAL.array($0.objectID, kAudioProcessPropertyDevices,
                                          scope: kAudioObjectPropertyScopeInput, of: AudioObjectID.self) }
            },
            subdevices: { device in
                let kind = HAL.value(device, kAudioObjectPropertyClass, default: UInt32(0))
                guard kind == kAudioAggregateDeviceClassID else { return nil }
                let members = HAL.array(device, kAudioAggregateDevicePropertyActiveSubDeviceList,
                                        of: AudioObjectID.self)
                // AudioSubDevice objects expose metadata, not input streams.
                // Translate their UIDs back to devices that an AudioUnit can open.
                var devices: [AudioObjectID] = []
                for member in members {
                    guard let uid = HAL.string(member, kAudioDevicePropertyDeviceUID),
                          let device = HAL.deviceObject(forUID: uid) else { return [] }
                    devices.append(device)
                }
                return devices
            },
            hasInput: { device in
                guard HAL.value(device, kAudioDevicePropertyDeviceIsAlive, default: UInt32(0)) != 0 else {
                    return false
                }
                return !HAL.array(device, kAudioDevicePropertyStreams,
                                  scope: kAudioObjectPropertyScopeInput, of: AudioObjectID.self).isEmpty
            },
            name: { HAL.string($0, kAudioObjectPropertyName) ?? "" })
    }
}

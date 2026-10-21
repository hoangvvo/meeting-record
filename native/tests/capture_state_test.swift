import CoreAudio
import Foundation

private struct Packet {
    let samples: [Float]
    let frames: UInt32
    let channels: UInt32
    let rate: Double
    let time: UInt64
}

private final class Output {
    var packets: [Packet] = []
}

private let record: MrecAudioCallback = { samples, frames, channels, rate, time, context in
    guard let samples, let context else { preconditionFailure("missing callback data") }
    let output = Unmanaged<Output>.fromOpaque(context).takeUnretainedValue()
    output.packets.append(Packet(samples: Array(UnsafeBufferPointer(start: samples,
                                                                   count: Int(frames * channels))),
                                 frames: frames, channels: channels, rate: rate, time: time))
}

private final class Buffers {
    private let storage: UnsafeMutableRawPointer
    private var samples: [UnsafeMutablePointer<Float>] = []
    let list: UnsafeMutablePointer<AudioBufferList>
    var buffers: UnsafeMutableAudioBufferListPointer { UnsafeMutableAudioBufferListPointer(list) }

    init(_ planes: [[Float]], channels: [UInt32]) {
        precondition(planes.count == channels.count)
        let size = MemoryLayout<AudioBufferList>.size
            + max(0, planes.count - 1) * MemoryLayout<AudioBuffer>.stride
        storage = .allocate(byteCount: size, alignment: MemoryLayout<AudioBufferList>.alignment)
        storage.initializeMemory(as: UInt8.self, repeating: 0, count: size)
        list = storage.bindMemory(to: AudioBufferList.self, capacity: 1)
        list.pointee.mNumberBuffers = UInt32(planes.count)
        for (index, plane) in planes.enumerated() {
            let data = UnsafeMutablePointer<Float>.allocate(capacity: max(1, plane.count))
            for (sample, value) in plane.enumerated() { data[sample] = value }
            samples.append(data)
            buffers[index] = AudioBuffer(mNumberChannels: channels[index],
                                          mDataByteSize: UInt32(plane.count * MemoryLayout<Float>.size),
                                          mData: data)
        }
    }

    deinit {
        for sample in samples { sample.deallocate() }
        storage.deallocate()
    }
}

private func state(channels: UInt32, capacity: UInt32 = 2) -> (CaptureStateBox, Output) {
    let state = CaptureStateBox()
    let output = Output()
    state.configure(callback: record, userData: Unmanaged.passUnretained(output).toOpaque())
    state.updateFormat(sampleRate: 48_000, channels: channels, maximumFrameCount: capacity)
    return (state, output)
}

@main struct CaptureStateTests {
    static func interleavedBuffersPreserveShapeAndClock() {
        let (state, output) = state(channels: 2)
        let source = Buffers([[1, 10, 2, 20]], channels: [2])
        let ticks = mach_absolute_time()
        state.emit(source.buffers, hostTime: ticks)
        precondition(output.packets.count == 1)
        let packet = output.packets[0]
        precondition(packet.samples == [1, 10, 2, 20])
        precondition(packet.frames == 2 && packet.channels == 2 && packet.rate == 48_000)
        var clock = mach_timebase_info_data_t()
        precondition(mach_timebase_info(&clock) == KERN_SUCCESS)
        precondition(packet.time == ticks / UInt64(clock.denom) * UInt64(clock.numer)
            + ticks % UInt64(clock.denom) * UInt64(clock.numer) / UInt64(clock.denom))
        state.clear()
    }

    static func planarBuffersSplitWithoutLosingFrames() {
        let (state, output) = state(channels: 2)
        let source = Buffers([[1, 2, 3, 4, 5], [10, 20, 30, 40, 50]], channels: [1, 1])
        state.emit(source.buffers, hostTime: mach_absolute_time())
        precondition(output.packets.map(\.frames) == [2, 2, 1])
        precondition(output.packets.flatMap(\.samples) == [1, 10, 2, 20, 3, 30, 4, 40, 5, 50])
        let first = output.packets[0].time
        var clock = mach_timebase_info_data_t()
        precondition(mach_timebase_info(&clock) == KERN_SUCCESS)
        let tick = Int64((UInt64(clock.numer) + UInt64(clock.denom) - 1) / UInt64(clock.denom))
        for (index, packet) in output.packets.enumerated() {
            let expected = Int64(Double(index * 2) / 48_000 * 1_000_000_000)
            precondition(abs(Int64(packet.time - first) - expected) <= tick + 1)
        }
        state.clear()
    }

    static func mixedChannelGroupsInterleave() {
        let (state, output) = state(channels: 3)
        let source = Buffers([[10, 11, 20, 21], [1, 2]], channels: [2, 1])
        state.emit(source.buffers, hostTime: nil)
        precondition(output.packets.count == 1)
        precondition(output.packets[0].samples == [10, 11, 1, 20, 21, 2])
        precondition(output.packets[0].channels == 3 && output.packets[0].time > 0)
        state.clear()
    }

    static func malformedBuffersDoNotReachConsumers() {
        let (state, output) = state(channels: 2)
        for source in [
            Buffers([], channels: []),
            Buffers([[]], channels: [2]),
            Buffers([[1, 2]], channels: [0]),
            Buffers([[1, 2]], channels: [1]),
            Buffers([[1, 2, 3]], channels: [2]),
            Buffers([[1, 2], [3]], channels: [1, 1]),
            Buffers([[1], [2, 3]], channels: [1, 2])
        ] {
            state.emit(source.buffers, hostTime: nil)
            precondition(output.packets.isEmpty, "malformed buffer was emitted")
        }
        let missing = Buffers([[1, 2], [3, 4]], channels: [1, 1])
        missing.buffers[1].mData = nil
        state.emit(missing.buffers, hostTime: nil)
        let partial = Buffers([[1, 2]], channels: [2])
        partial.buffers[0].mDataByteSize -= 1
        state.emit(partial.buffers, hostTime: nil)
        precondition(output.packets.isEmpty)
        state.clear()
    }

    static func clearAndReconfigureDiscardOldFormat() {
        let (state, output) = state(channels: 2)
        let old = Buffers([[1, 2], [3, 4]], channels: [1, 1])
        state.clear()
        state.emit(old.buffers, hostTime: nil)
        precondition(output.packets.isEmpty)
        precondition(state.currentFormat().channels == 0 && state.currentFormat().sampleRate == 0)
        state.configure(callback: record, userData: Unmanaged.passUnretained(output).toOpaque())
        state.updateFormat(sampleRate: 44_100, channels: 1)
        state.emit(old.buffers, hostTime: nil)
        let silent = Buffers([[0, 0]], channels: [1])
        state.emit(silent.buffers, hostTime: nil)
        precondition(output.packets.count == 1)
        precondition(output.packets[0].samples == [0, 0] && output.packets[0].rate == 44_100)
        state.clear()
    }

    static func invalidFormatsAndEmptyFramesAreRejected() {
        let (state, output) = state(channels: 1)
        let source = Buffers([[1, 2]], channels: [1])
        for rate in [0, -1, Double.infinity, Double.nan] {
            state.updateFormat(sampleRate: rate, channels: 1)
            state.emit(source.buffers, hostTime: nil)
        }
        state.updateFormat(sampleRate: 48_000, channels: 1)
        let data = source.buffers[0].mData!.assumingMemoryBound(to: Float.self)
        state.emit(data, frameCount: 0, channels: 1)
        state.emit(data, frameCount: 1, channels: 0)
        state.emit(data, frameCount: 1, channels: 2)
        precondition(output.packets.isEmpty)
        state.clear()
    }

    static func main() {
        interleavedBuffersPreserveShapeAndClock()
        planarBuffersSplitWithoutLosingFrames()
        mixedChannelGroupsInterleave()
        malformedBuffersDoNotReachConsumers()
        clearAndReconfigureDiscardOldFormat()
        invalidFormatsAndEmptyFramesAreRejected()
        print("capture state tests passed (6 cases)")
    }
}

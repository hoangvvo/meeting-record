import CoreAudio
import Foundation

private var emittedSamples: [Float] = []
private var emittedFrameCount: UInt32 = 0
private var emittedChannels: UInt32 = 0

private let captureTestCallback: MrecAudioCallback = {
    frames, frameCount, channels, _, _, _ in
    guard let frames else { return }
    emittedSamples = Array(UnsafeBufferPointer(
        start: frames, count: Int(frameCount * channels)))
    emittedFrameCount = frameCount
    emittedChannels = channels
}

/// Capture-core regressions that need neither an audio source nor TCC permission.
@main
struct TapDescriptionTests {
    static var failures = 0

    static func expect(_ condition: Bool, _ description: String) {
        if condition {
            print("  ok   \(description)")
        } else {
            print("  FAIL \(description)")
            failures += 1
        }
    }

    static func main() {
        do {
            let mono = try TapCapture.makeTapDescription(
                pids: [], globalMixdown: true, mono: true, muteCapturedOutput: false)
            expect(mono.isExclusive,
                   "mono system tap treats its empty process list as exclusions")
            expect(mono.processes.isEmpty, "mono system tap excludes no processes")

            let stereo = try TapCapture.makeTapDescription(
                pids: [], globalMixdown: true, mono: false, muteCapturedOutput: false)
            expect(stereo.isExclusive,
                   "stereo system tap treats its empty process list as exclusions")
            expect(stereo.processes.isEmpty, "stereo system tap excludes no processes")
        } catch {
            print("  FAIL constructing system tap descriptions: \(error)")
            failures += 1
        }

        print("\nnon-interleaved device buffers")
        let state = CaptureStateBox()
        state.configure(callback: captureTestCallback, userData: nil)
        state.updateFormat(sampleRate: 48_000, channels: 2, maximumFrameCount: 2)
        let byteCount = MemoryLayout<AudioBufferList>.size + MemoryLayout<AudioBuffer>.size
        let rawList = UnsafeMutableRawPointer.allocate(
            byteCount: byteCount, alignment: MemoryLayout<AudioBufferList>.alignment)
        rawList.initializeMemory(as: UInt8.self, repeating: 0, count: byteCount)
        let list = rawList.bindMemory(to: AudioBufferList.self, capacity: 1)
        list.pointee.mNumberBuffers = 2
        let buffers = UnsafeMutableAudioBufferListPointer(list)
        var left: [Float] = [1, 2]
        var right: [Float] = [10, 20]
        left.withUnsafeMutableBufferPointer { leftBuffer in
            right.withUnsafeMutableBufferPointer { rightBuffer in
                buffers[0] = AudioBuffer(
                    mNumberChannels: 1,
                    mDataByteSize: UInt32(leftBuffer.count * MemoryLayout<Float>.size),
                    mData: leftBuffer.baseAddress)
                buffers[1] = AudioBuffer(
                    mNumberChannels: 1,
                    mDataByteSize: UInt32(rightBuffer.count * MemoryLayout<Float>.size),
                    mData: rightBuffer.baseAddress)
                state.emit(buffers, hostTime: nil)
            }
        }
        expect(emittedSamples == [1, 10, 2, 20],
               "two mono HAL buffers are interleaved as stereo")
        expect(emittedFrameCount == 2 && emittedChannels == 2,
               "interleaved callback reports the original frame shape")
        rawList.deallocate()
        state.clear()

        print("\n\(failures == 0 ? "PASS" : "FAIL") — \(failures) failure(s)")
        exit(failures == 0 ? 0 : 1)
    }
}

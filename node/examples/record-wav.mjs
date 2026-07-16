// Record whatever is playing to a .wav file.
//
//   node node/examples/record-wav.mjs [seconds]
//
// Start some audio first.
import { writeFileSync } from 'node:fs'
import * as MeetingRecord from '../dist/index.js'

const seconds = Number(process.argv[2] ?? 10)

if (MeetingRecord.permissions.status('system-audio') !== 'granted') {
  console.log('requesting system audio permission — approve the dialog')
  const status = await MeetingRecord.permissions.request('system-audio')
  if (status !== 'granted') {
    console.error(`permission ${status}; cannot record`)
    process.exit(1)
  }
}

console.log('recording the system mix')

const session = await MeetingRecord.capture.start({ type: 'system' })
const track = session.systemAudio
console.log(`format: ${track.sampleRate}Hz ${track.channels}ch`)

const chunks = []
let dropped = 0
track.on('drop', (n) => (dropped += n))
track.on('data', (chunk) => chunks.push(chunk))

for (let i = seconds; i > 0; i--) {
  process.stdout.write(`\r${i}s remaining `)
  await new Promise((r) => setTimeout(r, 1000))
}
await session.stopRecording()
process.stdout.write('\r                    \r')

const pcm = Buffer.concat(chunks)
const samples = new Float32Array(pcm.buffer, pcm.byteOffset, pcm.length / 4)

let peak = 0
for (const s of samples) if (Math.abs(s) > peak) peak = Math.abs(s)

/** 16-bit PCM rather than float32; float WAV support is inconsistent. */
function wav(samples, sampleRate, channels) {
  const header = Buffer.alloc(44)
  const bytes = samples.length * 2
  header.write('RIFF', 0)
  header.writeUInt32LE(36 + bytes, 4)
  header.write('WAVE', 8)
  header.write('fmt ', 12)
  header.writeUInt32LE(16, 16) // fmt chunk size
  header.writeUInt16LE(1, 20) // PCM
  header.writeUInt16LE(channels, 22)
  header.writeUInt32LE(sampleRate, 24)
  header.writeUInt32LE(sampleRate * channels * 2, 28) // byte rate
  header.writeUInt16LE(channels * 2, 32) // block align
  header.writeUInt16LE(16, 34) // bits per sample
  header.write('data', 36)
  header.writeUInt32LE(bytes, 40)

  const body = Buffer.alloc(bytes)
  for (let i = 0; i < samples.length; i++) {
    // A tap can exceed 1.0 when processes are mixed; wrapping would click.
    const clamped = Math.max(-1, Math.min(1, samples[i]))
    body.writeInt16LE(Math.round(clamped * 32767), i * 2)
  }
  return Buffer.concat([header, body])
}

const file = `recording-${Date.now()}.wav`
writeFileSync(file, wav(samples, track.sampleRate, track.channels))

const duration = samples.length / track.channels / track.sampleRate
console.log(`wrote ${file}`)
console.log(`  ${duration.toFixed(1)}s, peak ${peak.toFixed(3)}${dropped ? `, dropped ${dropped}` : ''}`)
console.log(peak > 0.0001 ? `\nopen it:  open ${file}` : '\nsilent — is your output muted?')

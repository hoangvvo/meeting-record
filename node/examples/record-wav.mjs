// Record whatever is currently playing to a playable .wav file.
//
//   node node/examples/record-wav.mjs [seconds]
//
// Play music or a video first, then run this. Produces a WAV you can open in
// QuickTime — the quickest way to confirm capture actually works.
import { writeFileSync } from 'node:fs'
import mrec from '../dist/index.js'

const seconds = Number(process.argv[2] ?? 10)

if (mrec.permissions.status('system-audio') !== 'granted') {
  console.log('requesting system audio permission — approve the dialog')
  const status = await mrec.permissions.request('system-audio')
  if (status !== 'granted') {
    console.error(`permission ${status}; cannot record`)
    process.exit(1)
  }
}

const playing = mrec.meetings.audioProcesses().filter((p) => p.isPlayingAudio)
if (playing.length === 0) {
  console.error('nothing is playing audio right now — start some audio and retry')
  process.exit(1)
}
console.log(`recording: ${playing.map((p) => p.name).join(', ')}`)

const session = await mrec.capture.start({ pids: playing.map((p) => p.pid), mono: true })
console.log(`format: ${session.sampleRate}Hz ${session.channels}ch`)

const chunks = []
let dropped = 0
session.on('drop', (n) => (dropped += n))
session.on('data', (chunk) => chunks.push(chunk))

for (let i = seconds; i > 0; i--) {
  process.stdout.write(`\r${i}s remaining `)
  await new Promise((r) => setTimeout(r, 1000))
}
await session.stop()
process.stdout.write('\r                    \r')

const pcm = Buffer.concat(chunks)
const samples = new Float32Array(pcm.buffer, pcm.byteOffset, pcm.length / 4)

let peak = 0
for (const s of samples) if (Math.abs(s) > peak) peak = Math.abs(s)

/**
 * Write 16-bit PCM rather than float32: every player handles it, whereas
 * float WAV support is inconsistent.
 */
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
    // Clamp before scaling: a tap can legitimately exceed 1.0 when several
    // processes are mixed, and wrapping would sound like loud clicks.
    const clamped = Math.max(-1, Math.min(1, samples[i]))
    body.writeInt16LE(Math.round(clamped * 32767), i * 2)
  }
  return Buffer.concat([header, body])
}

const file = `recording-${Date.now()}.wav`
writeFileSync(file, wav(samples, session.sampleRate, session.channels))

const duration = samples.length / session.channels / session.sampleRate
console.log(`wrote ${file}`)
console.log(`  ${duration.toFixed(1)}s, peak ${peak.toFixed(3)}${dropped ? `, dropped ${dropped}` : ''}`)
console.log(peak > 0.0001 ? `\nopen it:  open ${file}` : '\nsilent — is your output muted?')

// Live meeting-detection monitor.
//
//   node node/examples/watch.mjs
//
// Prints detection state as it changes. Open Zoom / Teams / a Google Meet tab /
// a Slack huddle and watch the confidence climb. Needs no permission at all.
import mrec from '../dist/index.js'

const stamp = () => new Date().toLocaleTimeString()

function describe(m) {
  const bits = [
    m.isPlayingAudio ? 'remote audio' : null,
    m.isUsingMic ? 'mic live' : null,
  ].filter(Boolean)
  return [
    `${m.platform} (${m.appName}) pid=${m.pid}`,
    `  confidence ${m.confidence}${m.shouldRecord ? '  -> WOULD RECORD' : '  (below threshold)'}`,
    bits.length ? `  ${bits.join(', ')}` : '  no audio activity',
    m.title ? `  title: ${m.title}` : null,
    m.url ? `  url:   ${m.url}` : null,
  ]
    .filter(Boolean)
    .join('\n')
}

console.log('meeting-record — live monitor')
console.log(`accessibility: ${mrec.permissions.status('accessibility')} ` +
            `(titles and URLs are blank without it)`)
console.log('system audio:  ' + mrec.permissions.status('system-audio'))
console.log('\nwatching. open or join a call. ctrl-c to stop.\n')

const initial = mrec.meetings.scan()
if (initial.length === 0) {
  console.log('nothing detected right now')
} else {
  for (const m of initial) console.log(`[${stamp()}] already active\n${describe(m)}\n`)
}

mrec.meetings.on('started', (m) => console.log(`[${stamp()}] STARTED\n${describe(m)}\n`))
mrec.meetings.on('updated', (m) => console.log(`[${stamp()}] UPDATED\n${describe(m)}\n`))
mrec.meetings.on('ended', (m) => console.log(`[${stamp()}] ENDED   ${m.platform}\n`))
mrec.meetings.watch()

// Also show which processes are producing audio, since that is the signal that
// decides everything and it explains any surprising result.
setInterval(() => {
  const active = mrec.meetings
    .audioProcesses()
    .filter((p) => p.isPlayingAudio || p.isUsingMic)
  if (active.length) {
    const list = active
      .map((p) => `${p.name}${p.isUsingMic ? '(mic)' : ''}`)
      .join(', ')
    console.log(`[${stamp()}] audio active: ${list}`)
  }
}, 10_000)

process.on('SIGINT', () => {
  mrec.meetings.unwatch()
  console.log('\nstopped')
  process.exit(0)
})

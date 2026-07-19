import type { MeetingEvent, MeetingView, Meter, PermissionName, PermissionStatus } from './api'
import { bridge } from './bridge'

const PLATFORM_LABELS: Record<string, string> = {
  zoom: 'Zoom',
  teams: 'Microsoft Teams',
  meet: 'Google Meet',
  webex: 'Webex',
  slack: 'Slack',
  discord: 'Discord',
  browser: 'Browser',
  unknown: 'Unknown',
}

const LOG_LIMIT = 200

type RecordingState = 'idle' | 'starting' | 'recording' | 'stopping'

const meetings = new Map<string, MeetingView>()
let state: RecordingState = 'idle'

function el<T extends HTMLElement>(id: string): T {
  const node = document.getElementById(id)
  if (!node) throw new Error(`missing #${id}`)
  return node as T
}

const ui = {
  notices: el('notices'),
  elapsed: el('elapsed'),
  level: el('level'),
  record: el<HTMLButtonElement>('record'),
  stop: el<HTMLButtonElement>('stop'),
  microphone: el<HTMLInputElement>('microphone'),
  microphoneToggle: el<HTMLLabelElement>('microphone-toggle'),
  detail: el('detail'),
  meetings: el<HTMLUListElement>('meetings'),
  meetingsEmpty: el('meetings-empty'),
  log: el<HTMLOListElement>('log'),
}

interface PermissionUi {
  readonly name: PermissionName
  readonly label: string
  readonly ask: string
  readonly granted: string
  readonly denied: string
  readonly notice: HTMLElement
  readonly text: HTMLElement
  readonly button: HTMLButtonElement
}

const PERMISSIONS: readonly PermissionUi[] = [
  {
    name: 'system-audio',
    label: 'system audio',
    ask: 'Allow audio recording to capture what you hear.',
    granted: 'Audio recording is allowed.',
    denied: 'Audio recording is off. Turn it on in System Settings › Privacy & Security.',
    notice: el('audio-notice'),
    text: el('audio-text'),
    button: el('audio-request'),
  },
  {
    name: 'microphone',
    label: 'microphone',
    ask: 'Allow the microphone to record your own voice.',
    granted: 'Microphone is allowed.',
    denied: 'Microphone is off. Turn it on in System Settings › Privacy & Security.',
    notice: el('microphone-notice'),
    text: el('microphone-text'),
    button: el('microphone-request'),
  },
  {
    name: 'accessibility',
    label: 'accessibility',
    ask: 'Allow accessibility to read meeting titles and links.',
    granted: 'Accessibility is allowed.',
    denied: 'Accessibility is off. Turn it on in System Settings › Privacy & Security.',
    notice: el('accessibility-notice'),
    text: el('accessibility-text'),
    button: el('accessibility-request'),
  },
]

function log(text: string, kind: 'event' | 'error' = 'event'): void {
  const item = document.createElement('li')
  item.dataset.kind = kind

  const now = new Date()
  const stamp = document.createElement('time')
  stamp.textContent = [now.getHours(), now.getMinutes(), now.getSeconds()]
    .map((part) => String(part).padStart(2, '0'))
    .join(':')

  const body = document.createElement('span')
  body.textContent = text

  item.append(stamp, body)
  ui.log.prepend(item)
  while (ui.log.childElementCount > LOG_LIMIT) ui.log.lastElementChild?.remove()
}

function describe(error: unknown): string {
  return error instanceof Error ? error.message : String(error)
}

const PERMISSION_TEXT: Record<PermissionStatus, keyof Pick<PermissionUi, 'ask' | 'granted' | 'denied'>> = {
  unknown: 'ask',
  granted: 'granted',
  denied: 'denied',
  'not-required': 'granted',
}

// a want rather than the checkbox state: granting the permission later re-arms
// the mic track, but only if the user didn't turn it off themselves
let micWanted = true
let micStatus: PermissionStatus = 'unknown'

function syncMicrophone(): void {
  const allowed = micStatus === 'granted' || micStatus === 'not-required'
  ui.microphone.checked = allowed && micWanted
  ui.microphone.disabled = !allowed || state !== 'idle'
  ui.microphoneToggle.dataset.allowed = String(allowed)
}

function showPermission(permission: PermissionUi, status: PermissionStatus): void {
  permission.notice.hidden = status === 'not-required'
  permission.notice.dataset.status = status
  permission.text.textContent = permission[PERMISSION_TEXT[status]]
  // re-asking after a refusal never prompts again, so only System Settings helps
  permission.button.hidden = status !== 'unknown'
  permission.button.disabled = false
  ui.notices.hidden = PERMISSIONS.every((entry) => entry.notice.hidden)
  if (permission.name === 'microphone') {
    micStatus = status
    syncMicrophone()
  }
}

async function refreshPermissions(): Promise<void> {
  for (const permission of PERMISSIONS) {
    showPermission(permission, await bridge.permissionStatus(permission.name))
  }
}

for (const permission of PERMISSIONS) {
  permission.button.addEventListener('click', async () => {
    permission.button.disabled = true
    permission.text.textContent = 'Waiting for your answer…'
    try {
      const status = await bridge.requestPermission(permission.name)
      log(`${permission.label} permission: ${status}`)
    } catch (error) {
      log(`${permission.label} permission request failed: ${describe(error)}`, 'error')
    }
    await refreshPermissions()
    await refreshMeetings()
  })
}

function label(meeting: MeetingView): string {
  return PLATFORM_LABELS[meeting.platform] ?? meeting.platform
}

function line(className: string, text: string): HTMLElement {
  const node = document.createElement('div')
  node.className = className
  node.textContent = text
  return node
}

function row(meeting: MeetingView): HTMLLIElement {
  const item = document.createElement('li')
  item.className = 'meeting'

  const body = document.createElement('div')
  body.className = 'meeting-body'

  const head = document.createElement('div')
  const platform = document.createElement('span')
  platform.className = 'platform'
  platform.textContent = label(meeting)
  const app = document.createElement('span')
  app.className = 'app'
  app.textContent = meeting.appName
  head.append(platform, app)
  body.append(head)

  if (meeting.title) body.append(line('meeting-title', meeting.title))

  const signals = [meeting.url.replace(/^https?:\/\//, '')]
  if (meeting.isUsingMic) signals.push('mic')
  if (meeting.isPlayingAudio) signals.push('audio')
  signals.push(`confidence ${meeting.confidence}`)
  body.append(line('meeting-signals', signals.filter(Boolean).join(' · ')))

  const button = document.createElement('button')
  button.textContent = 'Record'
  if (meeting.shouldRecord) button.className = 'primary'
  button.disabled = state !== 'idle'
  button.addEventListener('click', () => void start(meeting.id))

  item.append(body, button)
  return item
}

function renderMeetings(): void {
  const sorted = [...meetings.values()].sort(
    (a, b) => Number(b.shouldRecord) - Number(a.shouldRecord) || b.confidence - a.confidence,
  )
  ui.meetings.replaceChildren(...sorted.map(row))
  ui.meetingsEmpty.hidden = sorted.length > 0
}

async function refreshMeetings(): Promise<void> {
  try {
    const found = await bridge.scan()
    meetings.clear()
    for (const meeting of found) meetings.set(meeting.id, meeting)
    renderMeetings()
  } catch (error) {
    log(`scan failed: ${describe(error)}`, 'error')
  }
}

function onMeetingEvent({ kind, meeting }: MeetingEvent): void {
  if (kind === 'ended') meetings.delete(meeting.id)
  else meetings.set(meeting.id, meeting)
  renderMeetings()

  const detail = meeting.shouldRecord ? ' (should record)' : ''
  log(`${kind} ${label(meeting)}: ${meeting.appName}, confidence ${meeting.confidence}${detail}`)
}

function setState(next: RecordingState): void {
  state = next
  const live = next === 'recording' || next === 'stopping'
  ui.record.hidden = live
  ui.record.disabled = next !== 'idle'
  ui.stop.hidden = !live
  ui.stop.disabled = next !== 'recording'
  ui.elapsed.dataset.live = String(live)
  for (const button of ui.meetings.querySelectorAll('button')) button.disabled = next !== 'idle'
  syncMicrophone()
}

function showMeter({ seconds, level }: Meter): void {
  const minutes = Math.floor(seconds / 60)
  const rest = Math.floor(seconds % 60)
  ui.elapsed.textContent = `${minutes}:${String(rest).padStart(2, '0')}`
  ui.level.style.width = `${Math.min(100, level * 100)}%`
}

function format(sampleRate: number, channels: number): string {
  const khz = (sampleRate / 1000).toFixed(1).replace(/\.0$/, '')
  const layout = channels === 1 ? 'mono' : channels === 2 ? 'stereo' : `${channels} channels`
  return `${khz} kHz · ${layout}`
}

function fileLink(path: string): HTMLButtonElement {
  const button = document.createElement('button')
  button.className = 'link'
  button.textContent = path.slice(path.lastIndexOf('/') + 1)
  button.title = path
  button.addEventListener('click', () => void bridge.reveal(path))
  return button
}

function detail(...parts: Array<string | Node>): void {
  ui.detail.replaceChildren(...parts)
}

async function start(meetingId: string | null): Promise<void> {
  if (state !== 'idle') return
  const meeting = meetingId === null ? undefined : meetings.get(meetingId)
  const what = meeting ? label(meeting) : 'playing audio'

  const withMic = ui.microphone.checked

  // first start can block ~6s while TCC makes up its mind
  setState('starting')
  ui.elapsed.textContent = '0:00'
  detail('Starting…')
  log(`starting capture of ${what}${withMic ? ' with the microphone' : ''}`)

  try {
    const started = await bridge.startRecording(meetingId, withMic)
    setState('recording')
    const tracks = started.microphone ? ' · system + mic' : ''
    detail(`${format(started.sampleRate, started.channels)}${tracks} · `, fileLink(started.path))
    log(`recording ${what} at ${Math.round(started.sampleRate)} Hz, ${started.channels}ch`)
  } catch (error) {
    setState('idle')
    detail(describe(error))
    log(`could not start: ${describe(error)}`, 'error')
  }
}

async function stop(): Promise<void> {
  if (state !== 'recording') return
  setState('stopping')
  detail('Saving…')
  try {
    const result = await bridge.stopRecording()
    const silent = result.peak <= 0.0001 ? ' · silent' : ''
    detail('Saved ', fileLink(result.path), ` · ${result.seconds.toFixed(1)}s${silent}`)
    const dropped = result.dropped > 0 ? `, dropped ${result.dropped} samples` : ''
    log(`wrote ${result.path} (${result.seconds.toFixed(1)}s, peak ${result.peak.toFixed(3)}${dropped})`)
    const stems = [result.systemPath, result.micPath].filter(Boolean).join(', ')
    log(`stems: ${stems}`)
  } catch (error) {
    detail(describe(error))
    log(`could not stop: ${describe(error)}`, 'error')
  }
  ui.level.style.width = '0'
  setState('idle')
}

ui.record.addEventListener('click', () => void start(null))
ui.stop.addEventListener('click', () => void stop())
ui.microphone.addEventListener('change', () => {
  micWanted = ui.microphone.checked
})

document.title = `meeting-record (${bridge.binding})`

bridge.onMeetingEvent(onMeetingEvent)
bridge.onMeter(showMeter)

// grants change behind our back in System Settings, so re-read on the way in
window.addEventListener('focus', () => void refreshPermissions())

await refreshPermissions()
await refreshMeetings()
log('watching')

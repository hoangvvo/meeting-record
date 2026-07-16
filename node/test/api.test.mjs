import assert from 'node:assert/strict'
import test from 'node:test'

import * as MeetingRecord from '../dist/index.js'

test('exports only the public namespaces and error classes', () => {
  assert.deepEqual(Object.keys(MeetingRecord).sort(), [
    'AlreadyRunningError',
    'CaptureError',
    'InternalError',
    'MeetingRecordError',
    'PermissionError',
    'UnsupportedOsError',
    'capture',
    'meetings',
    'permissions',
  ])
})

test('library errors can be caught specifically or together', () => {
  const error = new MeetingRecord.PermissionError('permission not granted', -2)
  assert(error instanceof MeetingRecord.PermissionError)
  assert(error instanceof MeetingRecord.MeetingRecordError)
  assert(error instanceof Error)
  assert.equal(error.code, -2)
})

test('invalid public inputs use TypeError without starting capture', async () => {
  assert.throws(() => MeetingRecord.permissions.status('camera'), TypeError)
  await assert.rejects(MeetingRecord.capture.start({ type: 'process', pid: 0 }), TypeError)
  await assert.rejects(
    MeetingRecord.capture.start({ type: 'system' }, { microphone: 'built-in' }),
    TypeError,
  )
  assert.equal(MeetingRecord.capture.running, false)
})

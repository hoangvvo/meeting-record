use std::{
    collections::VecDeque,
    sync::{
        atomic::{AtomicBool, AtomicUsize, Ordering},
        Arc, Condvar, Mutex,
    },
    thread::{self, JoinHandle},
    time::{Duration, Instant},
};

use crate::{mixer::PcmMixer, Error, QueuedChunk, TrackQueue, STATE_PAUSED};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct MixedAudioOptions {
    pub sample_rate: u32,
}

#[derive(Debug)]
pub struct MixedAudioChunk {
    /// Mono signed 16-bit little-endian PCM.
    pub pcm: Vec<u8>,
    /// RMS of the encoded samples, from zero to one.
    pub level: f32,
}

#[derive(Default)]
struct Output {
    chunks: VecDeque<MixedAudioChunk>,
    bytes: usize,
    finished: bool,
    abandoned: bool,
}

struct Shared {
    output: Mutex<Output>,
    wake: Condvar,
    microphone_muted: AtomicBool,
}

/// Optional mono s16le mix. Read until `None` to consume the final tail.
pub struct MixedAudio {
    sample_rate: u32,
    sources: Vec<Arc<TrackQueue>>,
    shared: Arc<Shared>,
    worker: Option<JoinHandle<()>>,
}

impl MixedAudio {
    pub(crate) fn new(
        sample_rate: u32,
        sources: Vec<Arc<TrackQueue>>,
        state: Arc<AtomicUsize>,
    ) -> Result<Self, Error> {
        if sample_rate == 0 || sources.is_empty() || sources.len() > 2 {
            return Err(Error::Capture("invalid mixed audio configuration".into()));
        }
        let shared = Arc::new(Shared {
            output: Mutex::new(Output::default()),
            wake: Condvar::new(),
            microphone_muted: AtomicBool::new(false),
        });
        let worker_shared = Arc::clone(&shared);
        let worker_sources = sources.clone();
        let worker = thread::Builder::new()
            .name("meeting-record-mix".into())
            .spawn(move || run(sample_rate, worker_sources, state, worker_shared))
            .map_err(|error| Error::Internal(error.to_string()))?;
        Ok(Self {
            sample_rate,
            sources,
            shared,
            worker: Some(worker),
        })
    }

    pub fn sample_rate(&self) -> u32 {
        self.sample_rate
    }

    pub fn channels(&self) -> u32 {
        1
    }

    /// Wait for the next mixed chunk, including any retained tail after stop.
    pub fn recv(&self) -> Option<MixedAudioChunk> {
        let mut output = self.shared.output.lock().unwrap_or_else(|e| e.into_inner());
        loop {
            if let Some(chunk) = output.chunks.pop_front() {
                output.bytes -= chunk.pcm.len();
                self.shared.wake.notify_all();
                return Some(chunk);
            }
            if output.finished || output.abandoned {
                return None;
            }
            output = self
                .shared
                .wake
                .wait(output)
                .unwrap_or_else(|e| e.into_inner());
        }
    }

    /// Affects newly mixed microphone samples; the raw track is unchanged.
    pub fn set_microphone_muted(&self, muted: bool) {
        self.shared.microphone_muted.store(muted, Ordering::Release);
        self.shared.wake.notify_all();
    }

    /// Interleaved input samples discarded by the bounded mix input queues.
    pub fn dropped_samples(&self) -> usize {
        self.sources.iter().fold(0usize, |total, source| {
            total.saturating_add(source.dropped.load(Ordering::Relaxed))
        })
    }
}

impl Drop for MixedAudio {
    fn drop(&mut self) {
        {
            let mut output = self.shared.output.lock().unwrap_or_else(|e| e.into_inner());
            output.abandoned = true;
            output.chunks.clear();
            self.shared.wake.notify_all();
        }
        if let Some(worker) = self.worker.take() {
            let _ = worker.join();
        }
    }
}

fn fill_pending(sources: &[Arc<TrackQueue>], pending: &mut [Option<QueuedChunk>]) {
    for (source, packet) in sources.iter().zip(pending) {
        if packet.is_none() {
            *packet = source.try_recv();
        }
    }
}

fn update_mute(mixer: &mut PcmMixer, source_count: usize, shared: &Shared) {
    if source_count > 1 {
        mixer.set_muted(1, shared.microphone_muted.load(Ordering::Acquire));
    }
}

fn run(
    sample_rate: u32,
    sources: Vec<Arc<TrackQueue>>,
    state: Arc<AtomicUsize>,
    shared: Arc<Shared>,
) {
    let mut mixer = PcmMixer::new(sample_rate, sources.len());
    let mut pending: Vec<Option<QueuedChunk>> = sources.iter().map(|_| None).collect();
    let mut segment = None;
    let mut flushed = false;
    let mut startup_started = Instant::now();
    let mut startup_complete = false;
    let capacity = sample_rate as usize * 2;
    loop {
        // Drain DSP output first. A full output queue suspends input consumption.
        let mut output = shared.output.lock().unwrap_or_else(|e| e.into_inner());
        if output.abandoned {
            return;
        }
        while output.bytes < capacity {
            let Some(chunk) = mixer.read() else { break };
            output.bytes += chunk.pcm.len();
            output.chunks.push_back(chunk);
            shared.wake.notify_all();
        }
        if output.bytes >= capacity {
            let _ = shared
                .wake
                .wait_timeout(output, Duration::from_millis(10))
                .unwrap_or_else(|e| e.into_inner());
            continue;
        }
        drop(output);

        fill_pending(&sources, &mut pending);
        // A newer packet proves resume's producer barrier has completed. Refill
        // earlier empty slots so an older packet cannot be overtaken here.
        if pending
            .iter()
            .flatten()
            .map(|packet| packet.segment)
            .min()
            .is_some_and(|next| segment != Some(next))
        {
            fill_pending(&sources, &mut pending);
        }
        if let Some(next) = pending.iter().flatten().map(|packet| packet.segment).min() {
            if segment != Some(next) {
                update_mute(&mut mixer, sources.len(), &shared);
                if segment.is_some() && !flushed {
                    mixer.finish_segment();
                }
                mixer.restart();
                segment = Some(next);
                flushed = false;
                startup_started = Instant::now();
                startup_complete = false;
                // Publish the previous segment's tail before accepting more input.
                continue;
            }
            if !startup_complete {
                let current = state.load(Ordering::Acquire);
                let boundary = current & 3 != 0
                    || current >> 2 > next
                    || sources
                        .iter()
                        .any(|source| source.closed.load(Ordering::Acquire));
                let all_present = pending
                    .iter()
                    .all(|packet| packet.as_ref().is_some_and(|packet| packet.segment == next));
                let publishing_boundary = boundary
                    && sources
                        .iter()
                        .any(|source| source.writing.load(Ordering::Acquire));
                if !all_present
                    && (publishing_boundary
                        || (!boundary && startup_started.elapsed() < Duration::from_millis(100)))
                {
                    let output = shared.output.lock().unwrap_or_else(|e| e.into_inner());
                    if output.abandoned {
                        return;
                    }
                    let _ = shared
                        .wake
                        .wait_timeout(output, Duration::from_millis(10))
                        .unwrap_or_else(|e| e.into_inner());
                    continue;
                }
                if boundary {
                    fill_pending(&sources, &mut pending);
                }
                let mut order: Vec<usize> = pending
                    .iter()
                    .enumerate()
                    .filter_map(|(source, packet)| {
                        packet
                            .as_ref()
                            .filter(|packet| packet.segment == next)
                            .map(|_| source)
                    })
                    .collect();
                order.sort_by_key(|source| pending[*source].as_ref().unwrap().audio.host_time_ns);
                update_mute(&mut mixer, sources.len(), &shared);
                for source in order {
                    mixer.write(source, &pending[source].take().unwrap().audio);
                }
                startup_complete = true;
                continue;
            }
            update_mute(&mut mixer, sources.len(), &shared);
            for (source, packet) in pending.iter_mut().enumerate() {
                if packet.as_ref().is_some_and(|packet| packet.segment == next) {
                    mixer.write(source, &packet.take().unwrap().audio);
                }
            }
            continue;
        }

        let current = state.load(Ordering::Acquire);
        let closed = sources
            .iter()
            .all(|source| source.closed.load(Ordering::Acquire));
        let boundary = closed
            || current & 3 == STATE_PAUSED
            || segment.is_some_and(|segment| current >> 2 > segment);
        if boundary
            && sources
                .iter()
                .all(|source| !source.writing.load(Ordering::Acquire))
        {
            // Recheck after the producer barrier before flushing a partial sample.
            fill_pending(&sources, &mut pending);
            if pending.iter().any(Option::is_some) {
                continue;
            }
            if !flushed {
                update_mute(&mut mixer, sources.len(), &shared);
                mixer.finish_segment();
                flushed = true;
                continue;
            }
            if closed {
                let mut output = shared.output.lock().unwrap_or_else(|e| e.into_inner());
                output.finished = true;
                shared.wake.notify_all();
                return;
            }
        }
        let output = shared.output.lock().unwrap_or_else(|e| e.into_inner());
        if output.abandoned {
            return;
        }
        let _ = shared
            .wake
            .wait_timeout(output, Duration::from_millis(10))
            .unwrap_or_else(|e| e.into_inner());
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{PacketMeta, STATE_STOPPED};
    use std::time::Instant;

    fn write(
        queue: &TrackQueue,
        state: &AtomicUsize,
        count: usize,
        rate: f64,
        value: f32,
        time: u64,
    ) {
        queue.write(
            &vec![value; count],
            PacketMeta {
                frame_count: count as u32,
                channels: 1,
                sample_rate: rate,
                host_time_ns: time,
                segment: state.load(Ordering::Acquire) >> 2,
                ..PacketMeta::default()
            },
            state,
        );
    }

    fn wait_for(mut ready: impl FnMut() -> bool) {
        let deadline = Instant::now() + Duration::from_secs(2);
        while !ready() {
            assert!(Instant::now() < deadline, "mixed audio worker timed out");
            thread::sleep(Duration::from_millis(2));
        }
    }

    fn collect(audio: &MixedAudio) -> Vec<i16> {
        let mut samples = Vec::new();
        while let Some(chunk) = audio.recv() {
            samples.extend(
                chunk
                    .pcm
                    .as_chunks::<2>()
                    .0
                    .iter()
                    .map(|bytes| i16::from_le_bytes([bytes[0], bytes[1]])),
            );
        }
        samples
    }

    #[test]
    fn stop_retains_tail_behind_bounded_output() {
        let state = Arc::new(AtomicUsize::new(0));
        let source = Arc::new(TrackQueue::new());
        write(&source, &state, 200, 100.0, 0.25, 0);
        let audio = MixedAudio::new(100, vec![Arc::clone(&source)], Arc::clone(&state)).unwrap();
        wait_for(|| audio.shared.output.lock().unwrap().bytes == 200);
        state.store(STATE_STOPPED, Ordering::Release);
        source.close();
        assert_eq!(collect(&audio), vec![8192; 200]);
    }

    #[test]
    fn queued_segments_keep_order_and_omit_pause_time() {
        let state = Arc::new(AtomicUsize::new(0));
        let sources = vec![Arc::new(TrackQueue::new()), Arc::new(TrackQueue::new())];
        for source in &sources {
            write(source, &state, 200, 100.0, 0.25, 0);
        }
        state.store(4, Ordering::Release);
        for source in &sources {
            write(source, &state, 100, 100.0, 0.5, 60_000_000_000);
            source.close();
        }
        state.store(4 | STATE_STOPPED, Ordering::Release);
        let audio = MixedAudio::new(100, sources, state).unwrap();
        let samples = collect(&audio);
        assert_eq!(samples.len(), 300);
        let first = (0.5 / 2.0_f64.sqrt() * 32767.0).round() as i16;
        let second = (1.0 / 2.0_f64.sqrt() * 32767.0).round() as i16;
        assert!(samples[..200].iter().all(|sample| *sample == first));
        assert!(samples[200..].iter().all(|sample| *sample == second));
    }

    #[test]
    fn pause_flushes_partial_resampler_before_resume() {
        let state = Arc::new(AtomicUsize::new(0));
        let source = Arc::new(TrackQueue::new());
        write(&source, &state, 3, 4.0, 0.25, 0);
        let audio = MixedAudio::new(2, vec![Arc::clone(&source)], Arc::clone(&state)).unwrap();
        state.store(STATE_PAUSED, Ordering::Release);
        wait_for(|| audio.shared.output.lock().unwrap().bytes == 4);
        state.store(4, Ordering::Release);
        write(&source, &state, 2, 4.0, 0.5, 60_000_000_000);
        state.store(4 | STATE_STOPPED, Ordering::Release);
        source.close();
        assert_eq!(collect(&audio), [8192, 8192, 16384]);
    }

    #[test]
    fn mute_applies_to_microphone_and_reports_input_drops() {
        let state = Arc::new(AtomicUsize::new(0));
        let sources = vec![Arc::new(TrackQueue::new()), Arc::new(TrackQueue::new())];
        let audio = MixedAudio::new(100, sources.clone(), Arc::clone(&state)).unwrap();
        audio.set_microphone_muted(true);
        write(&sources[0], &state, 10, 100.0, 0.25, 0);
        write(&sources[1], &state, 10, 100.0, 0.5, 0);
        sources[0].dropped.store(7, Ordering::Relaxed);
        sources[1].dropped.store(11, Ordering::Relaxed);
        state.store(STATE_STOPPED, Ordering::Release);
        for source in &sources {
            source.close();
        }
        assert_eq!(audio.dropped_samples(), 18);
        let expected = (0.25 / 2.0_f64.sqrt() * 32767.0).round() as i16;
        assert_eq!(collect(&audio), vec![expected; 10]);
    }

    #[test]
    fn drop_abandons_full_output_without_waiting_for_reader() {
        let state = Arc::new(AtomicUsize::new(0));
        let source = Arc::new(TrackQueue::new());
        write(&source, &state, 200, 100.0, 0.25, 0);
        let audio = MixedAudio::new(100, vec![source], state).unwrap();
        wait_for(|| audio.shared.output.lock().unwrap().bytes == 200);
        let started = Instant::now();
        drop(audio);
        assert!(started.elapsed() < Duration::from_secs(1));
    }

    #[test]
    fn startup_aligns_sources_by_timestamp_instead_of_queue_order() {
        let state = Arc::new(AtomicUsize::new(0));
        let sources = vec![Arc::new(TrackQueue::new()), Arc::new(TrackQueue::new())];
        write(&sources[0], &state, 10, 100.0, 0.25, 50_000_000);
        // Queue both first packets before starting the worker so scheduling
        // cannot let the missing-source deadline expire before the mic arrives.
        write(&sources[1], &state, 10, 100.0, 0.5, 0);
        let audio = MixedAudio::new(100, sources.clone(), Arc::clone(&state)).unwrap();
        state.store(STATE_STOPPED, Ordering::Release);
        for source in &sources {
            source.close();
        }
        let samples = collect(&audio);
        assert_eq!(samples.len(), 15);
        for (index, sample) in samples.iter().enumerate() {
            let value = if index < 5 {
                0.5
            } else if index < 10 {
                0.75
            } else {
                0.25
            };
            let expected = (value / 2.0_f64.sqrt() * 32767.0).round() as i16;
            assert!((i32::from(*sample) - i32::from(expected)).abs() <= 1);
        }
    }

    #[test]
    fn missing_source_does_not_block_startup_indefinitely() {
        let state = Arc::new(AtomicUsize::new(0));
        let sources = vec![Arc::new(TrackQueue::new()), Arc::new(TrackQueue::new())];
        write(&sources[0], &state, 10, 100.0, 0.25, 0);
        let started = Instant::now();
        let audio = MixedAudio::new(100, sources, state).unwrap();
        wait_for(|| audio.shared.output.lock().unwrap().bytes > 0);
        assert!(started.elapsed() >= Duration::from_millis(90));
        assert_eq!(audio.recv().unwrap().pcm.len(), 20);
    }
}

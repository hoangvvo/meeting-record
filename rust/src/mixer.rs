use std::collections::BTreeMap;

use crate::{AudioChunk, MixedAudioChunk};

struct Input {
    resampler: MonoResampler,
    cursor: Option<i64>,
    muted: bool,
}

/// Timestamp-aligned mono PCM, retaining silence sparsely between packets.
pub(crate) struct PcmMixer {
    sample_rate: u32,
    block_frames: i64,
    inputs: Vec<Input>,
    gain: f64,
    blocks: BTreeMap<i64, Vec<f32>>,
    origin: Option<u64>,
    origin_frame: i64,
    emitted: i64,
    end: i64,
    committed: i64,
}

impl PcmMixer {
    pub(crate) fn new(sample_rate: u32, source_count: usize) -> Self {
        assert!(sample_rate > 0 && source_count > 0);
        Self {
            sample_rate,
            block_frames: (f64::from(sample_rate) / 10.0).round().max(1.0) as i64,
            inputs: (0..source_count)
                .map(|_| Input {
                    resampler: MonoResampler::new(sample_rate),
                    cursor: None,
                    muted: false,
                })
                .collect(),
            gain: 1.0 / (source_count as f64).sqrt(),
            blocks: BTreeMap::new(),
            origin: None,
            origin_frame: 0,
            emitted: 0,
            end: 0,
            committed: 0,
        }
    }

    pub(crate) fn write(&mut self, source: usize, chunk: &AudioChunk) {
        let origin = *self.origin.get_or_insert(chunk.host_time_ns);
        let elapsed = i128::from(chunk.host_time_ns) - i128::from(origin);
        let mut position = self.origin_frame
            + (elapsed as f64 * f64::from(self.sample_rate) / 1e9 + 0.5).floor() as i64;
        // Skip gaps over a second when every source was absent.
        if position > self.end + i64::from(self.sample_rate) {
            self.finish_segment();
            self.restart();
            self.origin = Some(chunk.host_time_ns);
            position = self.origin_frame;
        }
        let samples = self.inputs[source].resampler.resample(chunk);
        let input = &mut self.inputs[source];
        // Absorb up to 20 ms of callback-clock jitter at packet joins.
        if input.cursor.is_none_or(|cursor| {
            (position - cursor).abs() as f64 > f64::from(self.sample_rate) / 50.0
        }) {
            input.cursor = Some(position);
        }
        self.append(source, &samples);
    }

    fn append(&mut self, source: usize, samples: &[f32]) {
        let input = &mut self.inputs[source];
        let Some(start) = input.cursor else { return };
        input.cursor = Some(start + samples.len() as i64);
        self.end = self.end.max(input.cursor.unwrap());
        if input.muted {
            return;
        }
        let skip = (self.emitted - start).max(0) as usize;
        for (index, sample) in samples.iter().enumerate().skip(skip) {
            let frame = start + index as i64;
            let block = self
                .blocks
                .entry(frame / self.block_frames)
                .or_insert_with(|| vec![0.0; self.block_frames as usize]);
            let offset = (frame % self.block_frames) as usize;
            block[offset] = (f64::from(block[offset]) + f64::from(*sample) * self.gain) as f32;
        }
    }

    pub(crate) fn set_muted(&mut self, source: usize, muted: bool) {
        self.inputs[source].muted = muted;
    }

    pub(crate) fn finish_segment(&mut self) {
        for source in 0..self.inputs.len() {
            let tail = self.inputs[source].resampler.flush();
            self.append(source, &tail);
        }
        self.committed = self.end;
    }

    pub(crate) fn restart(&mut self) {
        self.origin = None;
        self.origin_frame = self.end;
        for input in &mut self.inputs {
            input.cursor = None;
            input.resampler.reset();
        }
    }

    pub(crate) fn read(&mut self) -> Option<MixedAudioChunk> {
        let mut ready = self.committed;
        if let Some(lead) = self.inputs.iter().filter_map(|input| input.cursor).max() {
            // Inputs more than half a second behind contribute silence.
            let active = self
                .inputs
                .iter()
                .filter_map(|input| input.cursor)
                .filter(|cursor| (lead - cursor) as f64 <= f64::from(self.sample_rate) / 2.0)
                .min()
                .unwrap();
            ready = ready.max(active);
        }
        let end = ready.min(self.emitted + self.block_frames);
        if end <= self.emitted {
            return None;
        }
        let count = (end - self.emitted) as usize;
        let mut pcm = Vec::with_capacity(count * 2);
        let mut squares = 0.0;
        for frame in self.emitted..end {
            let sample = self
                .blocks
                .get(&(frame / self.block_frames))
                .map_or(0.0, |block| block[(frame % self.block_frames) as usize])
                .clamp(-1.0, 1.0);
            let scale = if sample < 0.0 { 32768.0 } else { 32767.0 };
            // Match JavaScript Math.round, including negative half values.
            let encoded = (f64::from(sample) * scale + 0.5).floor() as i16;
            pcm.extend_from_slice(&encoded.to_le_bytes());
            squares += (f64::from(encoded) / 32768.0).powi(2);
        }
        while self
            .blocks
            .first_key_value()
            .is_some_and(|(index, _)| (index + 1) * self.block_frames <= end)
        {
            self.blocks.pop_first();
        }
        self.emitted = end;
        Some(MixedAudioChunk {
            pcm,
            level: (squares / count as f64).sqrt() as f32,
        })
    }
}

struct MonoResampler {
    output_rate: u32,
    sample_rate: f64,
    step: f64,
    previous: f32,
    position: f64,
    sum: f64,
    covered: f64,
}

impl MonoResampler {
    fn new(output_rate: u32) -> Self {
        Self {
            output_rate,
            sample_rate: 0.0,
            step: 1.0,
            previous: 0.0,
            position: 0.0,
            sum: 0.0,
            covered: 0.0,
        }
    }

    fn reset(&mut self) {
        *self = Self::new(self.output_rate);
    }

    fn resample(&mut self, chunk: &AudioChunk) -> Vec<f32> {
        if chunk.sample_rate != self.sample_rate {
            self.reset();
            self.sample_rate = chunk.sample_rate;
            self.step = chunk.sample_rate / f64::from(self.output_rate);
        }
        let channels = chunk.channels.max(1) as usize;
        let input: Vec<f32> = chunk
            .frames
            .chunks_exact(channels)
            .map(|frame| {
                (frame.iter().map(|sample| f64::from(*sample)).sum::<f64>() / channels as f64)
                    as f32
            })
            .collect();
        if self.step == 1.0 || input.is_empty() {
            return input;
        }
        let mut output = Vec::new();
        if self.step > 1.0 {
            for sample in input {
                let mut remaining: f64 = 1.0;
                while remaining > 0.0 {
                    let taken = remaining.min(self.step - self.covered);
                    self.sum += f64::from(sample) * taken;
                    self.covered += taken;
                    remaining -= taken;
                    if self.covered >= self.step - 1e-9 {
                        output.push((self.sum / self.step) as f32);
                        self.sum = 0.0;
                        self.covered = 0.0;
                    }
                }
            }
        } else {
            let last = input.len() as f64 - 1.0;
            while self.position < last {
                let index = self.position.floor() as isize;
                let from = if index < 0 {
                    self.previous
                } else {
                    input[index as usize]
                };
                let to = input[(index + 1) as usize];
                output.push(
                    (f64::from(from)
                        + (f64::from(to) - f64::from(from)) * (self.position - index as f64))
                        as f32,
                );
                self.position += self.step;
            }
            self.position -= input.len() as f64;
            self.previous = *input.last().unwrap();
        }
        output
    }

    fn flush(&mut self) -> Vec<f32> {
        if self.step > 1.0 {
            let output = if self.covered > 0.0 {
                vec![(self.sum / self.covered) as f32]
            } else {
                Vec::new()
            };
            self.sum = 0.0;
            self.covered = 0.0;
            output
        } else {
            let count = (-self.position / self.step - 1e-9).ceil().max(0.0) as usize;
            self.position = 0.0;
            vec![self.previous; count]
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn chunk(frames: Vec<f32>, channels: u32, rate: f64, time: u64) -> AudioChunk {
        AudioChunk {
            frame_count: frames.len() as u32 / channels,
            frames,
            channels,
            sample_rate: rate,
            host_time_ns: time,
            dropped_samples: 0,
        }
    }

    fn drain(mixer: &mut PcmMixer) -> Vec<i16> {
        let mut samples = Vec::new();
        while let Some(chunk) = mixer.read() {
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
    fn long_gaps_in_every_source_do_not_backfill_silence() {
        let mut mixer = PcmMixer::new(100, 1);
        for timestamp in [0, 60_000_000_000] {
            mixer.write(
                0,
                &AudioChunk {
                    frames: vec![0.25; 10],
                    sample_rate: 100.0,
                    channels: 1,
                    frame_count: 10,
                    host_time_ns: timestamp,
                    dropped_samples: 0,
                },
            );
        }
        mixer.finish_segment();
        let mut bytes = 0;
        while let Some(chunk) = mixer.read() {
            bytes += chunk.pcm.len();
        }
        assert_eq!(bytes, 40);
    }

    #[test]
    fn aligns_rates_mutes_microphone_and_removes_pause() {
        let mut mixer = PcmMixer::new(24_000, 2);
        mixer.write(1, &chunk(vec![0.5; 240], 1, 24_000.0, 0));
        mixer.write(0, &chunk(vec![0.25; 480], 1, 48_000.0, 0));
        mixer.finish_segment();
        mixer.set_muted(1, true);
        mixer.restart();
        mixer.write(0, &chunk(vec![0.25; 960], 1, 48_000.0, 60_000_000_000));
        mixer.write(1, &chunk(vec![0.5; 480], 1, 24_000.0, 60_000_000_000));
        mixer.finish_segment();
        let samples = drain(&mut mixer);
        assert_eq!(samples.len(), 720);
        for (index, sample) in samples.iter().enumerate() {
            let value = if index < 240 { 0.75 } else { 0.25 };
            let expected = (value / 2.0_f64.sqrt() * 32767.0).round() as i16;
            assert!((i32::from(*sample) - i32::from(expected)).abs() <= 1);
        }
    }

    #[test]
    fn missing_source_does_not_remove_time_while_another_keeps_recording() {
        let mut mixer = PcmMixer::new(100, 2);
        mixer.write(1, &chunk(vec![0.25; 10], 1, 100.0, 0));
        for index in 0..=60 {
            mixer.write(0, &chunk(vec![0.25; 10], 1, 100.0, index * 100_000_000));
        }
        mixer.write(1, &chunk(vec![0.25; 10], 1, 100.0, 6_000_000_000));
        mixer.finish_segment();
        assert_eq!(drain(&mut mixer).len(), 610);
    }

    #[test]
    fn short_timestamp_gaps_insert_sparse_silence() {
        let mut mixer = PcmMixer::new(100, 1);
        mixer.write(0, &chunk(vec![0.5; 5], 1, 100.0, 1_000_000_000));
        mixer.write(0, &chunk(vec![0.5; 5], 1, 100.0, 1_500_000_000));
        mixer.finish_segment();
        let samples = drain(&mut mixer);
        assert_eq!(samples.len(), 55);
        assert!(samples[5..50].iter().all(|sample| *sample == 0));
    }

    #[test]
    fn earlier_source_timestamps_do_not_underflow() {
        let mut mixer = PcmMixer::new(100, 2);
        mixer.write(0, &chunk(vec![0.0; 10], 1, 100.0, u64::MAX - 100_000_000));
        mixer.write(1, &chunk(vec![0.5; 10], 1, 100.0, u64::MAX - 150_000_000));
        mixer.finish_segment();
        let samples = drain(&mut mixer);
        assert_eq!(samples.len(), 10);
        assert!(samples[..5].iter().all(|sample| *sample > 0));
        assert!(samples[5..].iter().all(|sample| *sample == 0));
    }

    #[test]
    fn flushes_upsampling_tail_once() {
        let mut mixer = PcmMixer::new(24_000, 1);
        mixer.write(0, &chunk(vec![0.25; 80], 1, 8_000.0, 0));
        mixer.finish_segment();
        mixer.finish_segment();
        assert_eq!(drain(&mut mixer), vec![8192; 240]);
    }

    #[test]
    fn clips_encodes_little_endian_and_reports_encoded_rms() {
        let mut mixer = PcmMixer::new(10, 1);
        mixer.write(0, &chunk(vec![-2.0, 2.0], 1, 10.0, 0));
        let negative = mixer.read().unwrap();
        assert_eq!(negative.pcm, [0, 128]);
        assert_eq!(negative.level, 1.0);
        let positive = mixer.read().unwrap();
        assert_eq!(positive.pcm, [255, 127]);
        assert!((positive.level - 32767.0 / 32768.0).abs() < 1e-6);
    }

    #[test]
    fn resampling_is_continuous_across_stereo_chunks() {
        let frames: Vec<f32> = (0..100)
            .flat_map(|index| [index as f32 / 100.0, -0.25])
            .collect();
        for rate in [8_000.0, 44_100.0, 48_000.0] {
            let mut whole = MonoResampler::new(24_000);
            let mut expected = whole.resample(&chunk(frames.clone(), 2, rate, 0));
            expected.extend(whole.flush());
            let mut split = MonoResampler::new(24_000);
            let mut actual = Vec::new();
            for packet in frames.chunks(14) {
                actual.extend(split.resample(&chunk(packet.to_vec(), 2, rate, 0)));
            }
            actual.extend(split.flush());
            assert_eq!(actual.len(), expected.len(), "rate {rate}");
            assert!(actual
                .iter()
                .zip(expected)
                .all(|(actual, expected)| (actual - expected).abs() < 1e-6));
        }
    }

    #[test]
    fn downsampling_averages_channels_and_partial_tail() {
        let mut resampler = MonoResampler::new(2);
        let output = resampler.resample(&chunk(vec![1.0, -1.0, 1.0, 1.0, 0.5, 0.5], 2, 4.0, 0));
        assert_eq!(output, [0.5]);
        assert_eq!(resampler.flush(), [0.5]);
        assert!(resampler.flush().is_empty());
    }
}

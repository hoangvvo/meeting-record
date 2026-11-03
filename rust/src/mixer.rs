use std::{ptr::NonNull, slice};

use crate::{sys, AudioChunk, MixedAudioChunk};

/// Owned adapter to the shared native PCM mixer. Calls stay on the mix worker.
pub(crate) struct PcmMixer(NonNull<sys::PcmMixer>);

impl PcmMixer {
    pub(crate) fn new(sample_rate: u32, source_count: usize) -> Self {
        Self(
            NonNull::new(unsafe { sys::mrec_pcm_mixer_create(sample_rate, source_count as u32) })
                .expect("could not initialize audio mixer"),
        )
    }

    pub(crate) fn write(&mut self, source: usize, chunk: &AudioChunk) {
        assert_eq!(
            unsafe {
                sys::mrec_pcm_mixer_write(
                    self.0.as_ptr(),
                    source as u32,
                    chunk.frames.as_ptr(),
                    chunk.frame_count,
                    chunk.channels,
                    chunk.sample_rate,
                    chunk.host_time_ns,
                )
            },
            0,
            "audio mixer write failed"
        );
    }

    pub(crate) fn set_muted(&mut self, source: usize, muted: bool) {
        assert_eq!(
            unsafe { sys::mrec_pcm_mixer_set_muted(self.0.as_ptr(), source as u32, muted as i32) },
            0,
            "audio mixer mute failed"
        );
    }

    pub(crate) fn finish_segment(&mut self) {
        assert_eq!(
            unsafe { sys::mrec_pcm_mixer_finish_segment(self.0.as_ptr()) },
            0,
            "audio mixer flush failed"
        );
    }

    pub(crate) fn restart(&mut self) {
        assert_eq!(
            unsafe { sys::mrec_pcm_mixer_restart(self.0.as_ptr()) },
            0,
            "audio mixer restart failed"
        );
    }

    pub(crate) fn read(&mut self) -> Option<MixedAudioChunk> {
        let mut pcm = std::ptr::null();
        let mut bytes = 0;
        let mut level = 0.0;
        let result =
            unsafe { sys::mrec_pcm_mixer_read(self.0.as_ptr(), &mut pcm, &mut bytes, &mut level) };
        assert!(result >= 0, "audio mixer read failed");
        (result == 1).then(|| MixedAudioChunk {
            // Native output remains valid until the next read or drop.
            pcm: unsafe { slice::from_raw_parts(pcm, bytes) }.to_vec(),
            level,
        })
    }
}

impl Drop for PcmMixer {
    fn drop(&mut self) {
        unsafe { sys::mrec_pcm_mixer_destroy(self.0.as_ptr()) };
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
    fn timestamp_gaps_retain_the_old_filter_tail_and_reset_filter_history() {
        for rate in [44_100.0, 48_000.0, 96_000.0] {
            let frames = (rate / 10.0) as usize;
            let mut mixer = PcmMixer::new(16_000, 1);
            mixer.write(0, &chunk(vec![0.25; frames], 1, rate, 0));
            mixer.write(0, &chunk(vec![0.5; frames], 1, rate, 500_000_000));
            mixer.finish_segment();
            let output = drain(&mut mixer);
            assert_eq!(output.len(), 9600, "duration at {rate} Hz");
            for (index, sample) in output.iter().enumerate() {
                let expected = if index < 1600 {
                    8192
                } else if index < 8000 {
                    0
                } else {
                    16384
                };
                assert_eq!(*sample, expected, "sample {index} at {rate} Hz");
            }
        }
    }
}

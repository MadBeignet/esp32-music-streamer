#pragma once

#include <AudioTools.h>
#include <AudioTools/AudioCodecs/AudioEncoded.h>
#include <AudioTools/AudioCodecs/CodecAACHelix.h>
#include <AudioTools/AudioCodecs/CodecMP3Helix.h>
#include <AudioTools/AudioCodecs/CodecFLACFoxen.h>
#include <AudioTools/AudioCodecs/ContainerM4A.h>
#include <AudioTools/AudioCodecs/MultiDecoder.h>
#include <AudioTools/Concurrency/LockFree/RingBufferSPSC.h>
#include <SdFat.h>
#include <esp_heap_caps.h>
#include <esp_memory_utils.h>
#include <freertos/semphr.h>
#include <atomic>
#include "PinConfig.h"

extern SemaphoreHandle_t sharedSpiMutex;
extern void audio_metadata(audio_tools::MetaDataType type, const char* value,
                           int length);
extern void audio_id3image(const char* info, const uint8_t* imageBuffer,
                           size_t imageSize);

// The decoder must never wait for the I2S DMA queue.  This adapter gives the
// decoder a bounded, PSRAM-backed SPSC queue and lets loop() drain it in small
// nonblocking writes.
class PcmRingOutput final : public audio_tools::AudioOutput {
public:
  static constexpr size_t Capacity = 1024 * 1024;
  static constexpr size_t DrainChunk = 2048;
  // Caps how many bytes a single drain() call will push to I2S. Without this,
  // once decode races far enough ahead of real-time to fill the ring (which
  // happens almost immediately for MP3/AAC - Helix decodes orders of
  // magnitude faster than real-time, unlike FLAC, which is slow enough to
  // self-pace and rarely fills the ring at all) drain()'s while loop keeps
  // finding new data to send indefinitely, since decode refills the ring
  // about as fast as this drains it. A hardware log showed a single loop()
  // call spanning 8.7 *seconds* this way: drain_ticks (loop()'s first
  // statement) froze for that whole stretch because the call never
  // returned, and - much worse - because applyPendingFormatChange()/drain()
  // run under drainMutex, any pending stop()/playPath() track-change on
  // Core 1 (which takes drainMutex with a blocking wait) couldn't take
  // effect until this entire call finally finished. That let whatever was
  // already playing keep audibly running in the background for many extra
  // seconds after a skip/navigate, then the next track's audio only started
  // once the mutex freed - very plausibly what read as tracks "racing"/
  // skipping ahead. Bounding this makes drain() return to AudioTask's outer
  // loop regularly regardless of backlog size, so track-change requests are
  // serviced within one drain chunk instead of an entire backlog.
  static constexpr size_t MaxBytesPerDrainCall = 16 * DrainChunk;

  explicit PcmRingOutput(I2SStream& output) : output_(output) {
    pcmRing_.setUsePSRAM(true);
  }

  bool begin() {
    return pcmRing_.resize(Capacity);
  }

  void reset() {
    pcmRing_.reset();
    pendingLen_ = 0;
  }

  // Decode (Core 1) and drain (Core 0) now run on separate cores, so a full
  // ring is drained independently and in real time - waiting here for space
  // is safe and is exactly the backpressure decode needs. (In the old
  // single-thread design, waiting would have deadlocked, since the same
  // thread that needed to drain the ring was the one blocked here; that is
  // why this used to silently drop the tail and lie about success. Without
  // real backpressure, decode - which is far faster than real-time playback
  // for compressed formats - filled the ring almost immediately and then
  // discarded nearly all subsequent decoded audio while the file position
  // kept advancing normally, which sounded like tracks racing through their
  // content far too quickly.) decodeStep()'s DecodeAheadMarginBytes check
  // keeps this from being hit in normal operation for reasonably-encoded
  // files; MaxWriteWaitMs just bounds the worst case (e.g. an unusually low
  // bitrate file whose single decoded chunk exceeds that margin) so decode
  // can't stall the UI thread indefinitely.
  static constexpr uint32_t MaxWriteWaitMs = 1000;

  void cancelWrites() {
    writesCancelled_.store(true, std::memory_order_release);
  }

  void resumeWrites() {
    writesCancelled_.store(false, std::memory_order_release);
  }

  size_t write(const uint8_t* data, size_t len) override {
    size_t offset = 0;
    const uint32_t waitStartMs = millis();
    while (offset < len) {
      // Stop discards the remainder; reporting it consumed prevents
      // decoder wrappers from retrying a cancelled write.
      if (writesCancelled_.load(std::memory_order_acquire)) return len;
      const int written = pcmRing_.writeArray(
          data + offset, static_cast<int>(len - offset));
      if (written > 0) {
        offset += static_cast<size_t>(written);
        continue;
      }
      if (millis() - waitStartMs > MaxWriteWaitMs) {
        const size_t remaining = len - offset;
        droppedBytes_.fetch_add(static_cast<uint32_t>(remaining),
                                std::memory_order_relaxed);
        overflowCount_.fetch_add(1, std::memory_order_relaxed);
        // Decoder wrappers use writeBlocking(), which retries partial
        // writes; report the complete request after recording the dropped
        // tail so a truly stuck drain thread can't hang decode forever.
        return len;
      }
      vTaskDelay(1);
    }
    return len;
  }

  int availableForWrite() override {
    return pcmRing_.availableForWrite();
  }

  // Called by decoders on the decode thread (Core 1). The actual ring/I2S
  // state change is applied later by applyPendingFormatChange() on the drain
  // thread (Core 0), which exclusively owns output_/pcmRing_ resets. This
  // avoids racing output_ against an in-flight write() from drain().
  void setAudioInfo(audio_tools::AudioInfo info) override {
    // Unconditional (not gated by AudioToolsLogger's level, which defaults to
    // Warning and silently swallows MP3DecoderHelix's own info-change log -
    // unlike AAC's, which uses LOGW and is always visible). Needed to see
    // what MP3 actually reports as its real sample_rate/channels/bits, since
    // that's been invisible in every hardware log so far.
    Serial.printf(
        "PcmRingOutput::setAudioInfo: rate=%d channels=%d bits=%d\n",
        info.sample_rate, info.channels, info.bits_per_sample);
    portENTER_CRITICAL(&formatMux_);
    pendingInfo_ = info;
    formatChangePending_ = true;
    portEXIT_CRITICAL(&formatMux_);
    audio_tools::AudioOutput::setAudioInfo(info);
  }

  // Applies the most recent pending format change. Must only be called from
  // the drain thread (Core 0), serialized with drain() by the caller.
  bool applyPendingFormatChange() {
    audio_tools::AudioInfo info;
    bool pending;
    portENTER_CRITICAL(&formatMux_);
    pending = formatChangePending_;
    info = pendingInfo_;
    formatChangePending_ = false;
    portEXIT_CRITICAL(&formatMux_);
    if (!pending) return false;

    const bool formatChanged =
        appliedInfo_.sample_rate != info.sample_rate ||
        appliedInfo_.channels != info.channels ||
        appliedInfo_.bits_per_sample != info.bits_per_sample;
    if (formatChanged) {
      pcmRing_.reset();
      // Timed diagnostic: drain_ticks has repeatedly frozen at 0 for many
      // consecutive seconds right around a format transition, meaning
      // AudioTask's loop() never returns from *some* call in this function
      // - every specific mechanism checked via source inspection (short
      // writes, reconfigure resetting counters, etc.) looked bounded on
      // paper, so this logs actual elapsed time around the two real
      // I2S-driver calls here to pinpoint which one (if either) is the
      // culprit on the next hardware run. Remove once root-caused.
      const uint32_t flushStartUs = micros();
      output_.flush();
      const uint32_t flushUs = micros() - flushStartUs;
      // Logs the previously-applied format alongside the new one so a
      // hardware log can show exactly which field(s) triggered this reset -
      // e.g. distinguishing a genuine new-track/rate change from a decoder
      // re-reporting a format that should already match what playPath()
      // primed via primeAppliedInfo().
      Serial.printf(
          "I2S format transition: %d Hz, %d ch, %d-bit (was %d Hz, %d ch, "
          "%d-bit) flush_us=%u\n",
          info.sample_rate, info.channels, info.bits_per_sample,
          appliedInfo_.sample_rate, appliedInfo_.channels,
          appliedInfo_.bits_per_sample,
          static_cast<unsigned>(flushUs));
    }
    const uint32_t setInfoStartUs = micros();
    output_.setAudioInfo(info);
    const uint32_t setInfoUs = micros() - setInfoStartUs;
    if (setInfoUs > 20000) {
      Serial.printf(
          "applyPendingFormatChange: output_.setAudioInfo() took %u us "
          "(rate=%d ch=%d bits=%d)\n",
          static_cast<unsigned>(setInfoUs), info.sample_rate, info.channels,
          info.bits_per_sample);
    }
    appliedInfo_ = info;
    return formatChanged;
  }

  // Records the format playPath() already applied directly to output_ (while
  // isPlaying is still false, so no concurrent drain() can be in flight) so
  // the first real decoder-reported format doesn't trigger a redundant reset.
  void primeAppliedInfo(audio_tools::AudioInfo info) { appliedInfo_ = info; }

  void flush() override {
    portENTER_CRITICAL(&formatMux_);
    formatChangePending_ = false;
    portEXIT_CRITICAL(&formatMux_);
    pcmRing_.reset();
    pendingLen_ = 0;
    output_.flush();
  }

  // Timed diagnostic wrapper around output_.write() (see the comment on
  // applyPendingFormatChange()'s flush()/setAudioInfo() timing above): the
  // real I2S write is bounded to ~100ms by setWaitTimeWriteMs(), so a single
  // call taking dramatically longer than that would prove the block is
  // happening here rather than somewhere else in loop(). Remove once
  // root-caused.
  size_t timedWrite(const uint8_t* data, size_t len) {
    const uint32_t startUs = micros();
    const size_t written = output_.write(data, len);
    const uint32_t elapsedUs = micros() - startUs;
    if (elapsedUs > 150000) {
      Serial.printf(
          "drain(): output_.write() took %u us (requested=%u written=%u)\n",
          static_cast<unsigned>(elapsedUs), static_cast<unsigned>(len),
          static_cast<unsigned>(written));
    }
    return written;
  }

  size_t drain() {
    size_t total = 0;
    const uint32_t drainStartUs = micros();
    const bool hadDataToSend = pcmRing_.available() > 0 || pendingLen_ > 0;
    // Flush any tail left over from a short write on a previous call
    // *before* pulling anything new from the ring - see the member
    // comment on pendingLen_ for why: output_.write() (I2S, bounded by
    // setWaitTimeWriteMs()) can legitimately accept less than requested
    // under ordinary backpressure now, and readArray() already
    // irreversibly removed those bytes from pcmRing_, so retrying here is
    // the only way to avoid permanently losing them (which previously
    // manifested as tracks audibly racing/skipping ahead of where they
    // should be, exactly like the write-side bug described above).
    if (pendingLen_ > 0) {
      const int freeBytes = output_.availableForWrite();
      if (freeBytes <= 0) {
        zeroFreeBreaks_.fetch_add(1, std::memory_order_relaxed);
        drainUs_.fetch_add(micros() - drainStartUs, std::memory_order_relaxed);
        stalledCalls_.fetch_add(1, std::memory_order_relaxed);
        return 0;
      }
      const size_t amount = min(static_cast<size_t>(freeBytes), pendingLen_);
      const size_t written = timedWrite(drainBuffer_ + pendingOffset_, amount);
      total += written;
      pendingOffset_ += written;
      pendingLen_ -= written;
      if (pendingLen_ > 0) {
        // Still couldn't flush the whole retained tail - leave it queued
        // and try again next call rather than reading more from the ring.
        // Not re-added to shortWriteBytes_ here: that counter tracks newly
        // discovered shortfalls (see the main loop below), not repeated
        // retries of the same still-pending bytes.
        consumedBytes_.fetch_add(static_cast<uint32_t>(total),
                                 std::memory_order_relaxed);
        drainUs_.fetch_add(micros() - drainStartUs, std::memory_order_relaxed);
        return total;
      }
    }
    while (pcmRing_.available() > 0 && total < MaxBytesPerDrainCall) {
      const int freeBytes = output_.availableForWrite();
      if (freeBytes <= 0) {
        // The real DMA-backed availableForWrite() (see I2SDriverESP32V1.h)
        // reported zero free space even though the ring still has data
        // queued - i.e. this call is bailing out early rather than
        // blocking inside output_.write(). Tracked separately from a
        // "stalled call" below so a hardware log can distinguish this
        // (DMA genuinely reporting full) from some other cause of zero
        // progress.
        zeroFreeBreaks_.fetch_add(1, std::memory_order_relaxed);
        break;
      }
      const size_t amount = min(
          static_cast<size_t>(freeBytes),
          min(DrainChunk, static_cast<size_t>(pcmRing_.available())));
      const int read = pcmRing_.readArray(drainBuffer_, static_cast<int>(amount));
      if (read <= 0) break;
      const size_t written = timedWrite(drainBuffer_, static_cast<size_t>(read));
      total += written;
      if (written != static_cast<size_t>(read)) {
        // Retain the undelivered tail (already irreversibly pulled out of
        // pcmRing_ by readArray() above) instead of discarding it - see
        // pendingLen_'s comment. It will be retried at the top of the next
        // drain() call, ahead of any newer ring data.
        pendingOffset_ = written;
        pendingLen_ = static_cast<size_t>(read) - written;
        shortWriteBytes_.fetch_add(static_cast<uint32_t>(pendingLen_),
                                   std::memory_order_relaxed);
        break;
      }
    }
    drainUs_.fetch_add(micros() - drainStartUs, std::memory_order_relaxed);
    consumedBytes_.fetch_add(static_cast<uint32_t>(total),
                             std::memory_order_relaxed);
    if (hadDataToSend && total == 0) {
      // This call had queued data available yet wrote nothing at all -
      // the signature behind the pcm_consumed-stuck-at-zero anomaly under
      // investigation. Counted per-call (not per-byte) so the perf window
      // shows how many consecutive/total calls made zero progress.
      stalledCalls_.fetch_add(1, std::memory_order_relaxed);
    }
    const size_t queued = static_cast<size_t>(pcmRing_.available());
    if (queued > highWaterBytes_.load(std::memory_order_relaxed)) {
      highWaterBytes_.store(queued, std::memory_order_relaxed);
    }
    return total;
  }

  uint32_t overflowCount() const {
    return overflowCount_.load(std::memory_order_relaxed);
  }
  uint32_t droppedBytes() const {
    return droppedBytes_.load(std::memory_order_relaxed);
  }
  uint32_t consumedBytes() const {
    return consumedBytes_.load(std::memory_order_relaxed);
  }
  uint32_t shortWriteBytes() const {
    return shortWriteBytes_.load(std::memory_order_relaxed);
  }
  // See drain()'s comments: zeroFreeBreaks counts calls that bailed out
  // because availableForWrite() reported no DMA space; stalledCalls counts
  // calls that had ring data queued but wrote nothing at all (for any
  // reason). Both reset per perf window like drainUs/highWater.
  uint32_t zeroFreeBreaks() const {
    return zeroFreeBreaks_.load(std::memory_order_relaxed);
  }
  uint32_t stalledCalls() const {
    return stalledCalls_.load(std::memory_order_relaxed);
  }
  size_t queuedBytes() { return static_cast<size_t>(pcmRing_.available()); }
  size_t capacityBytes() const { return Capacity; }
  size_t highWaterBytes() const {
    return highWaterBytes_.load(std::memory_order_relaxed);
  }
  uint32_t drainUs() const { return drainUs_.load(std::memory_order_relaxed); }
  void resetPerfWindow() {
    drainUs_.store(0, std::memory_order_relaxed);
    zeroFreeBreaks_.store(0, std::memory_order_relaxed);
    stalledCalls_.store(0, std::memory_order_relaxed);
    highWaterBytes_.store(static_cast<size_t>(pcmRing_.available()),
                          std::memory_order_relaxed);
  }
  bool isPsramBacked() {
    return pcmRing_.address() != nullptr &&
           esp_ptr_external_ram(pcmRing_.address());
  }

private:
  I2SStream& output_;
  audio_tools::RingBufferSPSC<uint8_t> pcmRing_;
  uint8_t drainBuffer_[DrainChunk];
  // A short output_.write() (I2S, bounded by setWaitTimeWriteMs()) can leave
  // some of the bytes already pulled out of pcmRing_ by readArray()
  // unwritten. Since that read is destructive (the ring can't "un-read"
  // them), the only way to avoid permanently losing that audio is to keep
  // it here and retry it at the start of the next drain() call, ahead of
  // any newer ring data - otherwise every such short write silently drops a
  // chunk of PCM, which sounds like the track skipping/racing ahead of
  // where it should be.
  size_t pendingOffset_ = 0;
  size_t pendingLen_ = 0;
  std::atomic<uint32_t> overflowCount_{0};
  std::atomic<uint32_t> droppedBytes_{0};
  std::atomic<uint32_t> consumedBytes_{0};
  std::atomic<uint32_t> shortWriteBytes_{0};
  std::atomic<uint32_t> drainUs_{0};
  std::atomic<uint32_t> zeroFreeBreaks_{0};
  std::atomic<uint32_t> stalledCalls_{0};
  std::atomic<size_t> highWaterBytes_{0};
  std::atomic<bool> writesCancelled_{true};
  // Deferred format-change state: written by setAudioInfo() on the decode
  // thread, consumed by applyPendingFormatChange() on the drain thread.
  portMUX_TYPE formatMux_ = portMUX_INITIALIZER_UNLOCKED;
  audio_tools::AudioInfo pendingInfo_{};
  bool formatChangePending_ = false;
  audio_tools::AudioInfo appliedInfo_{};
};

// Widens 16-bit PCM (Helix's MP3/AAC output) up to 32-bit before it reaches
// PcmRingOutput/I2S, so I2S always runs at a single, fixed bit depth no
// matter which codec is currently playing. Without this, switching between
// a FLAC track (Foxen always outputs 32-bit) and an MP3/AAC track (Helix
// always outputs 16-bit) changed I2SDriverESP32V1's bits_per_sample, and
// I2SStream::setAudioInfo() can only apply a sample-rate-only change in
// place (i2s_channel_reconfig_std_clock()) - a bits_per_sample change falls
// back to a full i2s.end()/i2s.begin(), tearing down and recreating the
// whole I2S channel (fresh pin/clock/DMA setup). That heavier reconfigure
// was the source of an audible pop/static right at the codec-type
// transition. Detecting 16-bit input (rather than checking file extension)
// means this works correctly regardless of which decoder actually produced
// the data.
class BitDepthUpconverter final : public audio_tools::AudioOutput {
public:
  // TEMPORARY diagnostic for the MP3/AAC "too fast" investigation: set to
  // true to bypass widening entirely and let 16-bit decoder output (and a
  // matching 16-bit I2S declaration) pass straight through, to empirically
  // check whether the widening step has any bearing on playback speed.
  // Remove this flag and the guarded branches once root-caused.
  // RESULT: confirmed NOT the cause - bypassing widening (native 16-bit
  // output all the way to I2S) still played at the same ~2x speed, so
  // this stays disabled going forward.
  static constexpr bool kDiagnosticBypassWidening = false;

  explicit BitDepthUpconverter(audio_tools::AudioOutput& output)
      : output_(output) {}

  void resetDiagnostics() {
    inputBytes_.store(0, std::memory_order_relaxed);
    outputBytes_.store(0, std::memory_order_relaxed);
    formatNotifications_.store(0, std::memory_order_relaxed);
    firstWrite_ = true;
  }

  void logDiagnostics() const {
    Serial.printf(
        "PCM track diagnostic: input_bytes=%lu output_bytes=%lu "
        "format_notifications=%lu\n",
        static_cast<unsigned long>(inputBytes_.load(std::memory_order_relaxed)),
        static_cast<unsigned long>(outputBytes_.load(std::memory_order_relaxed)),
        static_cast<unsigned long>(
            formatNotifications_.load(std::memory_order_relaxed)));
  }

  void setAudioInfo(audio_tools::AudioInfo info) override {
    widening_ = !kDiagnosticBypassWidening && info.bits_per_sample == 16;
    formatNotifications_.fetch_add(1, std::memory_order_relaxed);
    Serial.printf(
        "PCM input format diagnostic: rate=%d channels=%d bits=%d widening=%d\n",
        info.sample_rate, info.channels, info.bits_per_sample, widening_);
    audio_tools::AudioInfo outInfo = info;
    if (widening_) {
      outInfo.bits_per_sample = 32;
    }
    output_.setAudioInfo(outInfo);
    audio_tools::AudioOutput::setAudioInfo(info);
  }

  size_t write(const uint8_t* data, size_t len) override {
    if (len > 0 && firstWrite_) {
      firstWrite_ = false;
      const audio_tools::AudioInfo info = audioInfo();
      Serial.printf(
          "PCM first write diagnostic: rate=%d channels=%d bits=%d "
          "widening=%d bytes=%u\n",
          info.sample_rate, info.channels, info.bits_per_sample, widening_,
          static_cast<unsigned>(len));
    }
    inputBytes_.fetch_add(static_cast<uint32_t>(len), std::memory_order_relaxed);
    if (!widening_) {
      const size_t written = output_.write(data, len);
      outputBytes_.fetch_add(static_cast<uint32_t>(written),
                            std::memory_order_relaxed);
      return written;
    }
    // Decoders always hand us whole interleaved sample frames, never a
    // stray odd trailing byte, so len is always a multiple of sizeof(int16_t).
    size_t samplesRemaining = len / sizeof(int16_t);
    const int16_t* src = reinterpret_cast<const int16_t*>(data);
    while (samplesRemaining > 0) {
      const size_t batch = min(samplesRemaining, ScratchSamples);
      for (size_t i = 0; i < batch; ++i) {
        // Left-align into the upper 16 bits of a signed 32-bit sample - the
        // same left-aligned convention Foxen's native FLAC output already
        // uses (see CodecFLACFoxen.h's patched bit-depth handling), so I2S
        // sees a consistent sample format regardless of source codec.
        scratch_[i] = static_cast<int32_t>(src[i]) * 65536;
      }
      // PcmRingOutput::write() (this class's eventual output_) always
      // either fully accepts a call or blocks/drops after MaxWriteWaitMs -
      // it never returns a short count to retry - so a single call per
      // batch is enough here too.
      const size_t written = output_.write(
          reinterpret_cast<const uint8_t*>(scratch_), batch * sizeof(int32_t));
      outputBytes_.fetch_add(static_cast<uint32_t>(written),
                            std::memory_order_relaxed);
      src += batch;
      samplesRemaining -= batch;
    }
    // Report the original (pre-widening) length: that is how many source
    // bytes this call consumed, not how many bytes were produced downstream.
    return len;
  }

  int availableForWrite() override {
    const int inner = output_.availableForWrite();
    // Each 16-bit source sample expands to 32 bits (2x) while widening;
    // halve the downstream free-space report so a caller bounding writes by
    // this value can't overrun the ring once expanded.
    return widening_ ? inner / 2 : inner;
  }

private:
  static constexpr size_t ScratchSamples = 512;
  audio_tools::AudioOutput& output_;
  int32_t scratch_[ScratchSamples];
  bool widening_ = false;
  bool firstWrite_ = true;
  std::atomic<uint32_t> inputBytes_{0};
  std::atomic<uint32_t> outputBytes_{0};
  std::atomic<uint32_t> formatNotifications_{0};
};

class AudioEngine {
private:
  // Bumped from 8KB: profiling showed copies/s stuck at single digits
  // regardless of file sample rate, with wildly variable per-call latency
  // (tens of ms up to 1s+) - a pattern consistent with fixed per-operation
  // overhead (SD command/seek setup, FAT chain lookups) dominating over
  // actual transfer time. A larger buffer means fewer, bigger SD reads and
  // FLAC decode() invocations per second, amortizing that overhead. Both the
  // read-side buffer (PSRAM, below) and Foxen's write_buffer (internal RAM,
  // see patch_audio_tools.py) are sized from this constant.
  static constexpr size_t AudioCopyBufferSize = 32 * 1024;
  static constexpr size_t FlacOutputBufferSamples = 16 * 1024;
  // Pause decode once the PCM ring has less than this much free space,
  // rather than calling copier.copy() (which can decode a low-bitrate MP3's
  // 8KB compressed chunk into several hundred KB of PCM in one call). This
  // keeps PcmRingOutput::write()'s wait-for-space path a rare safety net
  // instead of the normal steady-state behavior, so decode - which also
  // drives touch/UI on this same core - doesn't routinely block for however
  // long real-time playback needs to drain a large chunk.
  static constexpr size_t DecodeAheadMarginBytes = 256 * 1024;
  static constexpr bool EnablePeriodicPerfLogging = false;
  SemaphoreHandle_t audioMutex = nullptr;
  I2SStream i2s;
  PcmRingOutput pcmOutput{i2s};
  BitDepthUpconverter bitDepthUpconverter{pcmOutput};
  VolumeStream volumeOut{bitDepthUpconverter};
  MP3DecoderHelix mp3Decoder;
  FLACDecoderFoxen flacDecoder{8 * 1024, 2};
  AACDecoderHelix aacDecoder;
  MultiDecoder m4aDecoder;
  ContainerM4A m4aContainer{m4aDecoder};
  MultiDecoder decoder;
  EncodedAudioStream decoderStream{&volumeOut, &decoder};
  audio_tools::MetaDataID3 id3Metadata;
  // Read (Core 1) and decode (Core 0) used to run sequentially inside one
  // decodeStep() call - a hardware log showed max_read_us + max_decode_us
  // (~105ms + ~50ms = ~155ms per 32KB chunk) added up to slightly *more*
  // real time per chunk than that chunk represents at 48kHz/24-in-32bit
  // playback, so the PCM ring slowly drained over a track even though
  // neither half was individually broken - a classic case of two
  // real-time-adjacent-but-not-quite-there stages needing to overlap
  // instead of run back-to-back. Decode is handed off from Core 1's read
  // via a double-buffered SPSC pipeline: Core 1 fills
  // readBuffers_[fillIndex_] while decode concurrently works through
  // readBuffers_[decodeIndex_] (the other slot), so wall-clock time per
  // chunk becomes max(read, decode) instead of read+decode. Both buffers
  // are allocated once from PSRAM in begin().
  //
  // Decode runs in its own dedicated FreeRTOS task pinned to Core 0
  // (decodeTaskLoop(), spawned in begin()) rather than being called inline
  // from loop()/AudioTask. An earlier version called decodeConsumeStep()
  // directly from AudioTask's own loop, after drain() - but a single FLAC
  // chunk decode can take 50-150ms+, and that blocked AudioTask's very next
  // drain() call for just as long, which is far longer than the I2S DMA's
  // ~53ms of buffered slack (see cfg.buffer_count below) and made stutter
  // *worse*, not better. Giving decode its own task at a strictly lower
  // priority than AudioTask means FreeRTOS's preemptive scheduler always
  // switches back to AudioTask the instant it is ready to run (e.g. its
  // vTaskDelay(1) elapses), interrupting decode mid-chunk if needed - so
  // drain keeps its old ~1ms cadence untouched, and decode simply uses
  // whatever Core 0 cycles AudioTask isn't using at that instant. This is
  // safe because PcmRingOutput's ring is a genuine SPSC (single-producer/
  // single-consumer) lock-free queue - decode's write() (producer) and
  // drain()'s readArray() (consumer) were already designed to run
  // concurrently from separate threads even before this change.
  static constexpr int PipelineDepth = 2;
  uint8_t* readBuffers_[PipelineDepth] = {nullptr, nullptr};
  // Bytes actually read into each buffer. Only ever written by Core 1
  // before xSemaphoreGive(bufferReadySem_), and only ever read by Core 0
  // after xSemaphoreTake(bufferReadySem_) returns - the semaphore itself
  // provides the memory                                                                                            barrier, so no atomics are needed here.
  size_t readBufferLen_[PipelineDepth] = {0, 0};
  int fillIndex_ = 0;    // Core 1-only: which slot to fill next.
  int decodeIndex_ = 0;  // Core 0-only: which slot to decode next.
  // Counting semaphores implementing the two-slot pipeline: bufferFreeSem_
  // starts at PipelineDepth (both slots empty/available to fill) and
  // bufferReadySem_ starts at 0 (nothing decoded yet).
  SemaphoreHandle_t bufferFreeSem_ = nullptr;
  SemaphoreHandle_t bufferReadySem_ = nullptr;
  // The dedicated, lower-priority-than-AudioTask decode task spawned in
  // begin(); see the PipelineDepth comment above for why decode needs its
  // own task instead of running inline from loop()/AudioTask.
  TaskHandle_t decodeTaskHandle_ = nullptr;
  // Cross-core track-finish handshake. The SD read side (Core 1) can only
  // observe "no more file data"; it must not declare the track finished
  // itself, since the decode side (Core 0) may still be working through
  // buffered-but-undecoded chunks. Core 1 sets readSideEof_ once the file
  // is exhausted; Core 0 sets decodeSideEof_ once it has both seen
  // readSideEof_ and drained every buffered chunk, which is the only point
  // it is safe for Core 1 to actually stop playback.
  std::atomic<bool> readSideEof_{false};
  std::atomic<bool> decodeSideEof_{false};
  // Covers decode admission and completion so stop cannot free buffers
  // while the worker is entering or executing a decode call.
  std::atomic<bool> decodeEnabled_{false};
  SemaphoreHandle_t decodeLifecycleMutex_ = nullptr;
  FsFile audioFile;
  String temporaryPath;
  // isPlaying/isPausedFlag are written on the decode thread (Core 1, via
  // playPath()/stop()/pauseResume()) and read on the drain thread (Core 0,
  // via loop()), so they must be atomic now that decode and drain run
  // concurrently on separate cores.
  std::atomic<bool> isPlaying{false};
  std::atomic<bool> isPausedFlag{false};
  bool trackFinished = false;
  // Real wall-clock start time of the current track (millis(), set right
  // after "Audio started" prints) - paired with the elapsed-time print at
  // EOF below to measure actual playback duration against the file's known
  // real duration. Diagnostic only for the MP3/AAC "too fast" investigation;
  // remove once root-caused.
  uint32_t trackStartMs_ = 0;
  uint32_t perfWindowStartMs = 0;
  uint32_t perfBytes = 0;
  uint32_t perfCopies = 0;
  uint32_t perfMaxCopyUs = 0;
  // Tracks time spent only waiting to acquire sharedSpiMutex (a subset of
  // perfMaxCopyUs), so a stall can be attributed to SPI bus contention with
  // the display (e.g. a slow album-art JPEG draw) versus genuine SD
  // read/decode compute time.
  uint32_t perfMaxSpiWaitUs = 0;
  // perfMaxReadUs is Core 1-only (the SD read itself). perfMaxDecodeUs is
  // now written by Core 0's decode step and read/reset by Core 1's perf
  // print/reset block, so it has to be atomic now that read and decode run
  // concurrently on separate cores (matching perfMax*Us's previous combined
  // meaning, this pair still lets a stall be attributed to SD I/O versus
  // decode compute).
  uint32_t perfMaxReadUs = 0;
  std::atomic<uint32_t> perfMaxDecodeUs{0};
  portMUX_TYPE volumeMux = portMUX_INITIALIZER_UNLOCKED;
  float requestedVolume = 1.0f;
  bool volumeUpdatePending = false;
  // Guards output_/pcmRing_ resets so the decode thread's track-transition
  // calls (playPath()/stopUnlocked()) never race the drain thread's
  // in-flight drain()/applyPendingFormatChange() calls. The drain side only
  // ever takes this non-blockingly, so the hot drain path never stalls.
  SemaphoreHandle_t drainMutex = nullptr;
  // Diagnostics for the still-unresolved "pcm_consumed frozen for many
  // seconds while the ring stays full" anomaly: drainLoopTicks_ counts
  // every AudioTask loop() iteration (so a hardware log can tell whether
  // AudioTask is even running during a stall); drainMutexMisses_ counts
  // iterations where the non-blocking drainMutex take failed (mutex held
  // elsewhere); drainSkippedNotPlaying_ counts iterations where the mutex
  // was acquired but isPlaying/isPausedFlag gated drain() off. Exactly one
  // of {a drain() call, a mutex miss, a not-playing skip} happens per tick,
  // so these three plus drain()'s own zeroFreeBreaks/stalledCalls fully
  // account for every AudioTask iteration.
  std::atomic<uint32_t> drainLoopTicks_{0};
  std::atomic<uint32_t> drainMutexMisses_{0};
  std::atomic<uint32_t> drainSkippedNotPlaying_{0};

  static void logMemory(const char* stage) {
    const size_t internalFree =
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t internalLargest =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t psramFree =
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const size_t psramLargest =
        heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    Serial.printf(
        "Audio memory %s: internal_free=%u internal_largest=%u "
        "psram_free=%u psram_largest=%u\n",
        stage, static_cast<unsigned>(internalFree),
        static_cast<unsigned>(internalLargest), static_cast<unsigned>(psramFree),
        static_cast<unsigned>(psramLargest));
  }

  float getRequestedVolume() {
    portENTER_CRITICAL(&volumeMux);
    const float volume = requestedVolume;
    portEXIT_CRITICAL(&volumeMux);
    return volume;
  }

  void applyPendingVolume() {
    bool apply = false;
    float volume = 1.0f;
    portENTER_CRITICAL(&volumeMux);
    if (volumeUpdatePending) {
      volume = requestedVolume;
      volumeUpdatePending = false;
      apply = true;
    }
    portEXIT_CRITICAL(&volumeMux);
    if (apply) {
      volumeOut.setVolume(volume);
    }
  }

  static uint32_t readBigEndian32(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) |
           static_cast<uint32_t>(data[3]);
  }

  bool isFastStartM4A(FsFile& file) {
    const uint64_t fileSize = file.fileSize();
    uint64_t offset = 0;
    uint8_t header[16];
    bool sawMoov = false;

    while (offset + 8 <= fileSize) {
      if (!file.seekSet(offset) || file.read(header, sizeof(header)) < 8) {
        return false;
      }

      const uint32_t size32 = readBigEndian32(header);
      const bool isMoov = memcmp(header + 4, "moov", 4) == 0;
      const bool isMdat = memcmp(header + 4, "mdat", 4) == 0;
      uint64_t boxSize = size32;
      uint64_t headerSize = 8;

      if (size32 == 1) {
        if (file.read(header + 8, 8) != 8) {
          return false;
        }
        boxSize = (static_cast<uint64_t>(readBigEndian32(header + 8)) << 32) |
                  readBigEndian32(header + 12);
        headerSize = 16;
      } else if (size32 == 0) {
        boxSize = fileSize - offset;
      }

      if (boxSize < headerSize || offset + boxSize > fileSize) {
        return false;
      }
      if (isMdat) {
        return sawMoov;
      }
      if (isMoov) {
        sawMoov = true;
      }
      offset += boxSize;
    }

    return false;
  }

  static bool hasM4AExtension(const char* filepath) {
    String path = filepath;
    path.toLowerCase();
    return path.endsWith(".m4a") || path.endsWith(".mp4");
  }

  static bool hasFlacExtension(const char* filepath) {
    String path = filepath;
    path.toLowerCase();
    return path.endsWith(".flac");
  }

  // Reads the real sample rate/channels straight from a FLAC file's
  // STREAMINFO metadata block (fixed-format, always the first block, right
  // after the 4-byte "fLaC" magic - see the FLAC format spec) so playPath()
  // can configure I2S correctly from the very first frame instead of
  // guessing 44100 Hz and correcting later via a live I2S rate change mid-
  // decode. That live change (I2SStream::setAudioInfo() -> i2s_set_clk(),
  // which internally stops/restarts the I2S peripheral) was found to cause
  // an audible skip/dropout right at the transition on real hardware - most
  // FLAC files on this project are 48/96 kHz, so nearly every FLAC track
  // hit this path. Returns false (leaving the caller's fallback in place)
  // if the file doesn't start with a well-formed STREAMINFO block.
  static bool readFlacStreamInfo(FsFile& file, uint32_t& sampleRate,
                                  uint8_t& channels, uint8_t& bitsPerSample) {
    uint8_t header[8 + 18];
    file.seekSet(0);
    if (file.read(header, sizeof(header)) != static_cast<int>(sizeof(header))) {
      return false;
    }
    if (memcmp(header, "fLaC", 4) != 0) {
      return false;
    }
    // header[4] bit 7 = last-metadata-block flag, bits 6-0 = block type;
    // type 0 is STREAMINFO, which the spec guarantees is always first.
    if ((header[4] & 0x7f) != 0) {
      return false;
    }
    const uint8_t* info = header + 8;
    sampleRate = (static_cast<uint32_t>(info[10]) << 12) |
                 (static_cast<uint32_t>(info[11]) << 4) |
                 (info[12] >> 4);
    channels = static_cast<uint8_t>(((info[12] >> 1) & 0x07) + 1);
    bitsPerSample = static_cast<uint8_t>(
        (((info[12] & 0x01) << 4) | (info[13] >> 4)) + 1);
    // Sanity-check against plausible audio ranges rather than trusting a
    // corrupt/unexpected file blindly.
    if (sampleRate < 1000 || sampleRate > 655350 || channels == 0 ||
        channels > 8 || bitsPerSample < 4 || bitsPerSample > 32) {
      return false;
    }
    return true;
  }

  static uint32_t readSynchsafe32(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0] & 0x7f) << 21) |
           (static_cast<uint32_t>(data[1] & 0x7f) << 14) |
           (static_cast<uint32_t>(data[2] & 0x7f) << 7) |
           static_cast<uint32_t>(data[3] & 0x7f);
  }

  // Walks a FLAC file's metadata block chain (the same chain
  // readFlacStreamInfo() only peeks the first entry of) looking for a
  // PICTURE block (type 6, see the FLAC format spec) and, if found, hands
  // its embedded image bytes to audio_id3image(). No-op (leaves any
  // previously-cleared AlbumArt state as-is) if the file has no PICTURE
  // block.
  void extractFlacArtwork(FsFile& file) {
    constexpr size_t MaxImageSize = 512 * 1024;
    const uint64_t fileSize = file.fileSize();
    uint8_t magic[4];
    if (!file.seekSet(0) || file.read(magic, sizeof(magic)) != sizeof(magic) ||
        memcmp(magic, "fLaC", 4) != 0) {
      file.seekSet(0);
      return;
    }

    uint64_t offset = 4;
    while (offset + 4 <= fileSize) {
      uint8_t blockHeader[4];
      if (!file.seekSet(offset) ||
          file.read(blockHeader, sizeof(blockHeader)) != sizeof(blockHeader)) {
        break;
      }
      const bool isLast = (blockHeader[0] & 0x80) != 0;
      const uint8_t blockType = blockHeader[0] & 0x7f;
      const uint32_t blockSize = (static_cast<uint32_t>(blockHeader[1]) << 16) |
                                 (static_cast<uint32_t>(blockHeader[2]) << 8) |
                                 blockHeader[3];
      const uint64_t blockDataOffset = offset + sizeof(blockHeader);
      if (blockDataOffset + blockSize > fileSize) break;

      if (blockType == 6) {
        // PICTURE block layout (all big-endian): picture type(4),
        // mime-type length(4) + mime-type bytes, description length(4) +
        // description bytes, width(4), height(4), depth(4), colors(4),
        // picture data length(4), picture data.
        uint8_t fixed[8];
        if (!file.seekSet(blockDataOffset) ||
            file.read(fixed, sizeof(fixed)) != sizeof(fixed)) {
          break;
        }
        const uint32_t mimeLen = readBigEndian32(fixed + 4);
        uint64_t cursor = blockDataOffset + sizeof(fixed) + mimeLen;
        uint8_t descLenBuf[4];
        if (cursor + 4 > fileSize || !file.seekSet(cursor) ||
            file.read(descLenBuf, sizeof(descLenBuf)) != sizeof(descLenBuf)) {
          break;
        }
        const uint32_t descLen = readBigEndian32(descLenBuf);
        cursor += sizeof(descLenBuf) + descLen;
        // Skip width/height/depth/colors (4 fields x 4 bytes each).
        cursor += 16;
        uint8_t dataLenBuf[4];
        if (cursor + 4 > fileSize || !file.seekSet(cursor) ||
            file.read(dataLenBuf, sizeof(dataLenBuf)) != sizeof(dataLenBuf)) {
          break;
        }
        const uint32_t dataLen = readBigEndian32(dataLenBuf);
        cursor += sizeof(dataLenBuf);
        if (dataLen == 0 || dataLen > MaxImageSize || cursor + dataLen > fileSize) {
          break;
        }

        uint8_t* image = static_cast<uint8_t*>(ps_malloc(dataLen));
        if (image == nullptr) break;
        if (!file.seekSet(cursor) ||
            file.read(image, dataLen) != dataLen) {
          free(image);
          break;
        }
        const bool png = dataLen >= 4 && image[0] == 0x89 &&
                         image[1] == 'P' && image[2] == 'N' && image[3] == 'G';
        Serial.printf("FLAC PICTURE artwork found: %u bytes\n",
                      static_cast<unsigned>(dataLen));
        audio_id3image(png ? "PNG" : "JPEG", image, dataLen);
        free(image);
        file.seekSet(0);
        return;
      }

      offset = blockDataOffset + blockSize;
      if (isLast) break;
    }
    file.seekSet(0);
  }

  // Locates a top-level MP4/M4A box (e.g. "moov") by walking the file's
  // box chain, mirroring the same walk isFastStartM4A()/createFastStartCopy()
  // already do for layout detection. Used to bound the artwork byte-scan
  // below to just the moov box instead of the whole (potentially huge) file.
  bool findTopLevelBox(FsFile& file, const char* fourcc, uint64_t& boxOffset,
                       uint64_t& boxSize) {
    const uint64_t fileSize = file.fileSize();
    uint64_t offset = 0;
    uint8_t header[16];
    while (offset + 8 <= fileSize) {
      if (!file.seekSet(offset) || file.read(header, 8) != 8) return false;
      const uint32_t size32 = readBigEndian32(header);
      uint64_t boxDataSize = size32;
      uint64_t headerSize = 8;
      if (size32 == 1) {
        if (file.read(header + 8, 8) != 8) return false;
        boxDataSize = readBigEndian64(header + 8);
        headerSize = 16;
      } else if (size32 == 0) {
        boxDataSize = fileSize - offset;
      }
      if (boxDataSize < headerSize || offset + boxDataSize > fileSize) {
        return false;
      }
      if (memcmp(header + 4, fourcc, 4) == 0) {
        boxOffset = offset;
        boxSize = boxDataSize;
        return true;
      }
      offset += boxDataSize;
    }
    return false;
  }

  // M4A/MP4 cover art (the "covr" atom, nested moov/udta/meta/ilst/covr/data)
  // is just raw JPEG/PNG bytes wrapped in a few levels of box headers we
  // don't otherwise need to parse for this - reportEmbeddedImage()'s
  // signature scan already knows how to pull the image out of an arbitrary
  // byte range, so just bound that scan to the moov box.
  void extractM4aArtwork(FsFile& file) {
    uint64_t moovOffset = 0, moovSize = 0;
    if (!findTopLevelBox(file, "moov", moovOffset, moovSize)) {
      file.seekSet(0);
      return;
    }
    reportEmbeddedImage(file, moovOffset, moovOffset + moovSize);
  }

  void reportEmbeddedImage(FsFile& file, uint64_t start, uint64_t end) {
    constexpr size_t ChunkSize = 4096;
    constexpr size_t MaxImageSize = 512 * 1024;
    uint8_t buffer[ChunkSize];
    uint8_t* image = nullptr;
    size_t imageSize = 0;
    bool collecting = false;
    bool png = false;
    uint8_t tail[8] = {};
    size_t tailSize = 0;

    for (uint64_t offset = start; offset < end;) {
      const size_t amount = min(static_cast<uint64_t>(ChunkSize), end - offset);
      if (!file.seekSet(offset) || file.read(buffer, amount) != amount) break;
      for (size_t i = 0; i < amount; ++i) {
        const uint8_t value = buffer[i];
        if (!collecting) {
          if (i + 1 < amount && value == 0xff && buffer[i + 1] == 0xd8) {
            collecting = true;
            png = false;
            image = static_cast<uint8_t*>(ps_malloc(MaxImageSize));
            if (image == nullptr) return;
            image[0] = 0xff;
            image[1] = 0xd8;
            imageSize = 2;
            ++i;
            continue;
          }
          if (i + 8 < amount && value == 0x89 && buffer[i + 1] == 'P' &&
              buffer[i + 2] == 'N' && buffer[i + 3] == 'G') {
            collecting = true;
            png = true;
            image = static_cast<uint8_t*>(ps_malloc(MaxImageSize));
            if (image == nullptr) return;
            const uint8_t signature[] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a,
                                         0x1a, 0x0a};
            memcpy(image, signature, sizeof(signature));
            imageSize = sizeof(signature);
            i += 7;
            continue;
          }
        } else if (imageSize < MaxImageSize) {
          image[imageSize++] = value;
          if ((!png && tailSize > 0 && tail[tailSize - 1] == 0xff &&
               value == 0xd9) ||
              (png && tailSize >= 3 && tail[tailSize - 3] == 'I' &&
               tail[tailSize - 2] == 'E' && tail[tailSize - 1] == 'N' &&
               value == 'D')) {
            Serial.printf("Embedded artwork found: %u bytes\n",
                          static_cast<unsigned>(imageSize));
            audio_id3image(png ? "PNG" : "JPEG", image, imageSize);
            free(image);
            file.seekSet(0);
            return;
          }
          if (tailSize < sizeof(tail)) {
            tail[tailSize++] = value;
          } else {
            memmove(tail, tail + 1, sizeof(tail) - 1);
            tail[sizeof(tail) - 1] = value;
          }
        } else {
          free(image);
          return;
        }
      }
      offset += amount;
    }
    if (image != nullptr) free(image);
    file.seekSet(0);
  }

  void extractMp3Artwork(FsFile& file) {
    uint8_t header[10];
    if (!file.seekSet(0) || file.read(header, sizeof(header)) != sizeof(header) ||
        memcmp(header, "ID3", 3) != 0) {
      return;
    }

    const uint32_t tagSize = readSynchsafe32(header + 6);
    constexpr size_t MaxArtworkScan = 512 * 1024;
    const size_t scanSize = min(static_cast<size_t>(tagSize) + 10,
                                MaxArtworkScan);
    uint8_t* tag = static_cast<uint8_t*>(ps_malloc(scanSize));
    if (tag == nullptr || !file.seekSet(0) ||
        file.read(tag, scanSize) != scanSize) {
      if (tag != nullptr) free(tag);
      return;
    }

    const bool version24 = tag[3] == 4;
    size_t offset = 10;
    while (offset + (version24 ? 10 : 10) <= scanSize &&
           offset < static_cast<size_t>(tagSize) + 10) {
      const uint8_t* frame = tag + offset;
      if (frame[0] == 0) break;
      const uint32_t frameSize = version24
          ? readSynchsafe32(frame + 4)
          : readBigEndian32(frame + 4);
      if (frameSize == 0 || offset + 10 + frameSize > scanSize) break;

      if (memcmp(frame, "APIC", 4) == 0 && frameSize > 4) {
        const uint8_t* payload = frame + 10;
        size_t remaining = frameSize;
        const uint8_t encoding = *payload++;
        --remaining;
        while (remaining > 0 && *payload != 0) {
          ++payload;
          --remaining;
        }
        if (remaining > 0) {
          ++payload;
          --remaining;
        }
        if (remaining > 0) {
          ++payload;
          --remaining;
        }
        const size_t terminator = encoding == 1 || encoding == 2 ? 2 : 1;
        while (remaining >= terminator) {
          bool ended = true;
          for (size_t i = 0; i < terminator; ++i) {
            if (payload[i] != 0) ended = false;
          }
          if (ended) {
            payload += terminator;
            remaining -= terminator;
            break;
          }
          payload += terminator;
          remaining -= terminator;
        }
        if (remaining > 0 && (payload[0] == 0xff || payload[0] == 0x89)) {
          Serial.printf("ID3 APIC artwork found: %u bytes\n",
                        static_cast<unsigned>(remaining));
          audio_id3image("APIC", payload, remaining);
        }
        break;
      }
      offset += 10 + frameSize;
    }
    free(tag);
    file.seekSet(0);
  }

  static uint64_t readBigEndian64(const uint8_t* data) {
    return (static_cast<uint64_t>(readBigEndian32(data)) << 32) |
           readBigEndian32(data + 4);
  }

  static void writeBigEndian32(uint8_t* data, uint32_t value) {
    data[0] = value >> 24;
    data[1] = value >> 16;
    data[2] = value >> 8;
    data[3] = value;
  }

  static void writeBigEndian64(uint8_t* data, uint64_t value) {
    writeBigEndian32(data, value >> 32);
    writeBigEndian32(data + 4, value);
  }

  bool patchChunkOffsets(uint8_t* data, size_t length, int64_t delta) {
    size_t offset = 0;
    while (offset + 8 <= length) {
      uint64_t boxSize = readBigEndian32(data + offset);
      size_t headerSize = 8;
      if (boxSize == 1) {
        if (offset + 16 > length) return false;
        boxSize = readBigEndian64(data + offset + 8);
        headerSize = 16;
      } else if (boxSize == 0) {
        boxSize = length - offset;
      }
      if (boxSize < headerSize || boxSize > length - offset) return false;

      if (memcmp(data + offset + 4, "stco", 4) == 0) {
        if (boxSize < 16) return false;
        uint32_t count = readBigEndian32(data + offset + headerSize + 4);
        size_t entry = offset + headerSize + 8;
        if (entry + static_cast<size_t>(count) * 4 > offset + boxSize) {
          return false;
        }
        for (uint32_t i = 0; i < count; ++i, entry += 4) {
          int64_t adjusted = static_cast<int64_t>(
              readBigEndian32(data + entry)) + delta;
          if (adjusted < 0 || adjusted > UINT32_MAX) return false;
          writeBigEndian32(data + entry, static_cast<uint32_t>(adjusted));
        }
      } else if (memcmp(data + offset + 4, "co64", 4) == 0) {
        if (boxSize < 20) return false;
        uint32_t count = readBigEndian32(data + offset + headerSize + 4);
        size_t entry = offset + headerSize + 8;
        if (entry + static_cast<size_t>(count) * 8 > offset + boxSize) {
          return false;
        }
        for (uint32_t i = 0; i < count; ++i, entry += 8) {
          int64_t adjusted = static_cast<int64_t>(
              readBigEndian64(data + entry)) + delta;
          if (adjusted < 0) return false;
          writeBigEndian64(data + entry, static_cast<uint64_t>(adjusted));
        }
      } else if (boxSize > headerSize) {
        if (!patchChunkOffsets(data + offset + headerSize,
                               boxSize - headerSize, delta)) {
          return false;
        }
      }
      offset += boxSize;
    }
    return offset == length;
  }

  bool copyRange(FsFile& source, FsFile& destination, uint64_t offset,
                 uint64_t length) {
    uint8_t buffer[4096];
    while (length > 0) {
      size_t amount = length > sizeof(buffer) ? sizeof(buffer) : length;
      if (!source.seekSet(offset) || source.read(buffer, amount) != amount ||
          destination.write(buffer, amount) != amount) {
        return false;
      }
      offset += amount;
      length -= amount;
    }
    return true;
  }

  bool createFastStartCopy(const char* filepath, FsFile& source) {
    extern SdFat sd;
    const uint64_t fileSize = source.fileSize();
    uint64_t moovOffset = 0, moovSize = 0, moovHeaderSize = 8;
    uint64_t mdatOffset = 0, mdatSize = 0;
    uint64_t offset = 0;
    uint8_t header[16];
    while (offset + 8 <= fileSize) {
      if (!source.seekSet(offset) || source.read(header, 8) != 8) return false;
      uint32_t size32 = readBigEndian32(header);
      uint64_t boxSize = size32;
      uint64_t headerSize = 8;
      if (size32 == 1) {
        if (source.read(header + 8, 8) != 8) return false;
        boxSize = readBigEndian64(header + 8);
        headerSize = 16;
      } else if (size32 == 0) {
        boxSize = fileSize - offset;
      }
      if (boxSize < headerSize || offset + boxSize > fileSize) return false;
      if (memcmp(header + 4, "moov", 4) == 0) {
        moovOffset = offset;
        moovSize = boxSize;
        moovHeaderSize = headerSize;
      } else if (memcmp(header + 4, "mdat", 4) == 0) {
        mdatOffset = offset;
        mdatSize = boxSize;
      }
      offset += boxSize;
    }
    if (moovSize == 0 || mdatSize == 0 || moovOffset < mdatOffset ||
        moovSize > SIZE_MAX) {
      return false;
    }

    uint8_t* moov = static_cast<uint8_t*>(ps_malloc(moovSize));
    if (moov == nullptr || !source.seekSet(moovOffset) ||
        source.read(moov, moovSize) != moovSize) {
      if (moov) free(moov);
      return false;
    }

    String path = filepath;
    temporaryPath = path + ".faststart.tmp";
    sd.remove(temporaryPath.c_str());
    FsFile output = sd.open(temporaryPath.c_str(), O_RDWR | O_CREAT | O_TRUNC);
    if (!output) {
      free(moov);
      temporaryPath = "";
      return false;
    }

    uint64_t newMdatOffset = mdatOffset + moovSize;
    if (!patchChunkOffsets(moov + moovHeaderSize,
                           moovSize - moovHeaderSize,
                           static_cast<int64_t>(newMdatOffset + 8) -
                               static_cast<int64_t>(mdatOffset + 8)) ||
        !copyRange(source, output, 0, mdatOffset) ||
        output.write(moov, moovSize) != moovSize ||
        !copyRange(source, output, mdatOffset, mdatSize)) {
      output.close();
      sd.remove(temporaryPath.c_str());
      temporaryPath = "";
      free(moov);
      return false;
    }
    output.close();
    free(moov);
    source.close();
    audioFile = sd.open(temporaryPath.c_str(), O_READ);
    return static_cast<bool>(audioFile);
  }

public:
  void begin(int bclk, int lrc, int dout) {
    logMemory("before init");
    audioMutex = xSemaphoreCreateMutex();
    if (audioMutex == nullptr) {
      Serial.println("Audio mutex allocation failed!");
      return;
    }
    drainMutex = xSemaphoreCreateMutex();
    if (drainMutex == nullptr) {
      Serial.println("Audio drain mutex allocation failed!");
      return;
    }
    decodeLifecycleMutex_ = xSemaphoreCreateMutex();
    if (decodeLifecycleMutex_ == nullptr) {
      Serial.println("Audio decode lifecycle mutex allocation failed!");
      return;
    }
    for (int i = 0; i < PipelineDepth; ++i) {
      readBuffers_[i] = static_cast<uint8_t*>(ps_malloc(AudioCopyBufferSize));
      if (readBuffers_[i] == nullptr) {
        Serial.println("Audio read buffer allocation failed!");
        return;
      }
    }
    // bufferFreeSem_ starts full (both slots free/empty) and
    // bufferReadySem_ starts empty (nothing decoded yet) - see the
    // PipelineDepth comment above for how these two semaphores hand off
    // buffers between Core 1 (read) and Core 0 (decode).
    bufferFreeSem_ = xSemaphoreCreateCounting(PipelineDepth, PipelineDepth);
    bufferReadySem_ = xSemaphoreCreateCounting(PipelineDepth, 0);
    if (bufferFreeSem_ == nullptr || bufferReadySem_ == nullptr) {
      Serial.println("Audio pipeline semaphore allocation failed!");
      return;
    }
    // Pinned to Core 0 (same core as AudioTask/drain) but at a strictly
    // lower priority, so FreeRTOS always preempts this task in favor of
    // AudioTask the instant it is ready to run - see the PipelineDepth
    // comment above for why decode must never be able to delay drain.
    if (xTaskCreatePinnedToCore(decodeTaskTrampoline, "AudioDecodeTask",
                                16384, this, 1, &decodeTaskHandle_,
                                0) != pdPASS) {
      Serial.println("Audio decode task creation failed!");
      return;
    }
    auto cfg = i2s.defaultConfig(TX_MODE);
    cfg.sample_rate = 44100;
    cfg.channels = 2;
    // Start at 16-bit while idle - I2S's bit clock frequency scales with
    // bits_per_sample (sample_rate * channels * bits_per_sample), so a
    // 32-bit idle config continuously clocks at 2x the frequency this one
    // does, even while outputting silence. That extra idle-time toggling
    // turned out to be enough EMI to trip the MPR121's very sensitive touch
    // thresholds with phantom presses while just browsing (no track
    // playing). playPath() always requests 32-bit once a track is chosen
    // (see BitDepthUpconverter's comment for why), so this only costs one
    // restart the first time something is ever played, not one on every
    // FLAC<->MP3/AAC transition thereafter.
    cfg.bits_per_sample = 16;
    cfg.pin_bck = bclk;
    cfg.pin_ws = lrc;
    cfg.pin_data = dout;
    cfg.buffer_size = 1024;
    // dma_buf_len is capped at 1024 by the legacy ESP-IDF I2S driver, so add
    // margin via more buffers instead. At 96kHz/32-bit stereo (768,000
    // bytes/s) 20 buffers was only ~26ms of slack; 40 buffers gives ~53ms to
    // absorb SD/decode stalls without an I2S underrun.
    cfg.buffer_count = 40;
    if (!i2s.begin(cfg)) {
      Serial.println("I2S initialization failed!");
      return;
    }
    // The underlying ESP-IDF driver's writeBytes() defaults to
    // portMAX_DELAY, i.e. i2s_channel_write() can block forever if its
    // internal sent/written byte bookkeeping (used by availableForWrite())
    // ever desyncs from the real DMA queue state - which a hardware log
    // showed happening right at a sample-rate-only reconfigure (FLAC's
    // 48kHz -> AAC's 44.1kHz), since changeSampleRate()'s
    // i2s_channel_reconfig_std_clock() path does not reset those counters
    // the way flush() does. Since drain() calls this write from inside
    // loop() while holding drainMutex, an indefinite block there froze the
    // entire higher-priority AudioTask (drain_ticks/drain_mutex_miss/
    // drain_skip_not_playing all reading 0 for many consecutive seconds -
    // i.e. loop() wasn't merely failing to progress, it was never
    // returning from this single call at all) which is what caused the
    // observed multi-second stalls/skips. Bounding the wait means a
    // desynced/slow DMA queue now produces an ordinary short write
    // (already handled below via shortWriteBytes_/the next drain() call
    // retrying) instead of hanging the whole pipeline.
    auto* esp32Driver =
        static_cast<audio_tools::I2SDriverESP32V1*>(i2s.driver());
    if (esp32Driver != nullptr) {
      esp32Driver->setWaitTimeWriteMs(100);
    }
    if (!pcmOutput.begin()) {
      Serial.println("PCM ring allocation failed!");
      i2s.end();
      return;
    }
    Serial.printf(
        "Audio pipeline: 16-bit idle / 32-bit during playback "
        "(MP3/AAC upconverted), read=Core1, "
        "decode=Core0(low-prio task), drain=Core0(AudioTask), chunk=%u\n",
        static_cast<unsigned>(AudioCopyBufferSize));
    volumeOut.begin();
    id3Metadata.setCallback(audio_metadata);
    id3Metadata.setFilter(audio_tools::SELECT_ID3);
    id3Metadata.begin();
    flacDecoder.setInBufferSize(AudioCopyBufferSize);
    flacDecoder.setOutBufferSize(FlacOutputBufferSamples);
    flacDecoder.set32Bit(true);
    // Let the first AAC frame propagate its actual sample rate/channels to
    // I2S. I2SStream ignores repeated notifications when the format is
    // unchanged, so this does not restart the hardware for every frame.
    aacDecoder.setAudioInfoNotifications(true);
    // Ignore non-audio MP4 atoms such as large embedded cover-art payloads.
    // The demuxer still handles the audio sample tables and mdat stream.
    m4aContainer.getDemuxer().getParser().setCallback(
        [](MP4Parser::Box&, void*) {});
    // Keep the complete stsz table available. Artwork-bearing M4As can have
    // tens of thousands of AAC samples, and incremental parser chunks can
    // otherwise reach mdat before the demuxer has finalized the table.
    m4aContainer.getDemuxer().getParser().resize(64 * 1024);
    m4aDecoder.addDecoder(aacDecoder, "audio/aac");
    decoder.addDecoder(mp3Decoder, "audio/mpeg");
    decoder.addDecoder(flacDecoder, "audio/flac");
    decoder.addDecoder(m4aContainer, "audio/mp4");
    decoder.addDecoder(m4aContainer, "audio/m4a");
    logMemory("after init");
    Serial.printf("PSRAM PCM ring: capacity=%u bytes, external=%d\n",
                  static_cast<unsigned>(PcmRingOutput::Capacity),
                  pcmOutput.isPsramBacked());
  }

  void playPath(const char* filepath) {
    stop();
    trackFinished = false;
    
    extern SdFat sd;
    audioFile = sd.open(filepath, O_READ);
    if (!audioFile) {
      Serial.println("Failed to open audio file from SD!");
      return;
    }

    // max_read_us has been the dominant cost in recent perf logs (far above
    // max_decode_us, and climbing/fluctuating through a single track) -
    // exactly the signature of a fragmented file forcing extra FAT
    // chain-walk/seek work, or an SD card doing internal
    // housekeeping/GC stalls. Logging contiguity and cluster size up front
    // lets a hardware log directly confirm or rule out fragmentation as the
    // cause. contiguousRange() walks the file's whole FAT chain once here
    // (cheap relative to per-track playback), not per read.
    {
      Sector_t bgnSector = 0, endSector = 0;
      const bool contiguous = audioFile.contiguousRange(&bgnSector, &endSector);
      // sd.bytesPerCluster() truncates to 0 for 64KiB+ clusters (a common
      // size on large FAT32-formatted SD cards) because the underlying
      // FatVolume implementation returns a uint16_t that overflows. Compute
      // it from sectorsPerCluster() instead (SD/SDHC/SDXC cards always use
      // 512-byte logical sectors) so large clusters log correctly.
      const uint32_t bytesPerCluster =
          static_cast<uint32_t>(sd.sectorsPerCluster()) * 512;
      Serial.printf(
          "Audio file layout: contiguous=%d bytes_per_cluster=%lu fat_type=%u\n",
          contiguous ? 1 : 0, static_cast<unsigned long>(bytesPerCluster),
          static_cast<unsigned>(sd.fatType()));
    }

    if (hasM4AExtension(filepath) && !isFastStartM4A(audioFile)) {
      Serial.println("M4A is not fast-start; creating temporary SD copy...");
      if (!createFastStartCopy(filepath, audioFile)) {
        Serial.println("M4A rewrite failed; file cannot be played.");
        audioFile.close();
        return;
      }
    }
    audioFile.seekSet(0);

    audio_tools::AudioInfo initialInfo;
    initialInfo.sample_rate = 44100;
    initialInfo.channels = 2;
    // I2S always runs at 32-bit now (see BitDepthUpconverter) regardless of
    // the source codec's native bit depth, so this never varies by file
    // type - only sample_rate/channels get refined below for FLAC.
    initialInfo.bits_per_sample = 32;
    if (hasFlacExtension(filepath)) {
      // Read the file's actual sample rate/channels up front so I2S starts
      // configured correctly - see readFlacStreamInfo()'s comment for why
      // this avoids a mid-decode I2S rate change that was causing an
      // audible skip right after playback started on 48/96 kHz files.
      uint32_t flacSampleRate = 0;
      uint8_t flacChannels = 0;
      uint8_t flacBitsPerSample = 0;
      if (readFlacStreamInfo(audioFile, flacSampleRate, flacChannels,
                              flacBitsPerSample)) {
        initialInfo.sample_rate = static_cast<int>(flacSampleRate);
        initialInfo.channels = flacChannels;
      } else {
        Serial.println(
            "FLAC STREAMINFO parse failed; falling back to 44100 Hz guess.");
      }
      audioFile.seekSet(0);
    }
    Serial.printf("Preparing output format: rate=%d channels=%d bits=%d\n",
                  initialInfo.sample_rate, initialInfo.channels,
                  initialInfo.bits_per_sample);
    // isPlaying is still false here (stop() above cleared it), so the drain
    // thread's loop() will not be draining; drainMutex still guards against
    // its non-blocking applyPendingFormatChange() check running concurrently.
    if (drainMutex != nullptr) {
      xSemaphoreTake(drainMutex, portMAX_DELAY);
    }
    i2s.setAudioInfo(initialInfo);
    pcmOutput.reset();
    pcmOutput.primeAppliedInfo(initialInfo);
    if (drainMutex != nullptr) {
      xSemaphoreGive(drainMutex);
    }

    id3Metadata.begin();
    uint8_t metadataBuffer[1024];
    const size_t metadataBytes = audioFile.read(
        metadataBuffer, sizeof(metadataBuffer));
    if (metadataBytes > 0) {
      id3Metadata.write(metadataBuffer, metadataBytes);
    }
    audioFile.seekSet(0);
    if (hasFlacExtension(filepath)) {
      extractFlacArtwork(audioFile);
    } else if (hasM4AExtension(filepath)) {
      extractM4aArtwork(audioFile);
    } else {
      extractMp3Artwork(audioFile);
    }
    
    // FoxenFLACDecoder uses the write-based decoder interface. Keep the
    // encoded copy buffer bounded so SD reads do not consume task stack/heap.
    flacDecoder.setInBufferSize(AudioCopyBufferSize);
    flacDecoder.setOutBufferSize(FlacOutputBufferSamples);
    // ContainerM4A keeps sample-table storage across decoder lifetimes.
    // Reset it explicitly because automatic album continuation starts a new
    // file without reconstructing the decoder object.
    m4aContainer.getDemuxer().begin();
    bitDepthUpconverter.resetDiagnostics();
    // Decoder notifications deduplicate against a default 44.1kHz/16-bit
    // format. Seed every PCM stage before decode so that default-format
    // tracks cannot bypass widening, including after a native 32-bit FLAC.
    audio_tools::AudioInfo inputInfo = initialInfo;
    inputInfo.bits_per_sample = hasFlacExtension(filepath) ? 32 : 16;
    volumeOut.setAudioInfo(inputInfo);
    decoderStream.setOutput(volumeOut);
    if (!decoderStream.begin()) {
      Serial.println("Failed to initialize FLAC decoder!");
      audioFile.close();
      return;
    }
    logMemory("after decoder init");
    Serial.printf("Audio staging buffers: stream_copy=%u bytes (PSRAM allocator), "
                  "flac_input=%u bytes, flac_output=%u samples (%u bytes)\n",
                  static_cast<unsigned>(AudioCopyBufferSize),
                  static_cast<unsigned>(AudioCopyBufferSize),
                  static_cast<unsigned>(FlacOutputBufferSamples),
                  static_cast<unsigned>(FlacOutputBufferSamples * sizeof(int32_t)));
    Serial.printf("Audio started: %s (%lu bytes, pos=%lu, available=%d)\n",
                  filepath, static_cast<unsigned long>(audioFile.fileSize()),
                  static_cast<unsigned long>(audioFile.curPosition()),
                  audioFile.available());
    trackStartMs_ = millis();
    perfWindowStartMs = millis();
    perfBytes = 0;
    perfCopies = 0;
    perfMaxCopyUs = 0;
    perfMaxSpiWaitUs = 0;
    perfMaxReadUs = 0;
    perfMaxDecodeUs.store(0, std::memory_order_relaxed);
    pcmOutput.resetPerfWindow();
    // Pipeline state should already be at its rest position (both slots
    // free, nothing queued) as a postcondition of the stop() that ran at
    // the top of playPath(), but reset explicitly here too so a fresh
    // track never inherits stale indices/flags from whatever the previous
    // track left behind.
    fillIndex_ = 0;
    decodeIndex_ = 0;
    readSideEof_.store(false, std::memory_order_relaxed);
    decodeSideEof_.store(false, std::memory_order_relaxed);
    isPlaying = true;
    isPausedFlag = false;
    // Enabling decode last, only once isPlaying/pipeline state above are
    // fully settled, means Core 0's decodeConsumeStep() never observes a
    // half-initialized track.
    pcmOutput.resumeWrites();
    decodeEnabled_.store(true, std::memory_order_release);
  }

  // Core 0 (AudioTask): drain only. Decode runs in its own dedicated,
  // lower-priority task (decodeTaskLoop(), spawned in begin()) so a long
  // FLAC chunk decode can never delay this loop's next drain() call - see
  // the PipelineDepth comment for why an earlier version that called
  // decode inline from here made stutter worse, not better. drainMutex is
  // still taken non-blockingly so a rare, brief contention with the decode
  // thread's track-transition calls just skips this tick's drain/
  // format-apply instead of stalling the DMA feed.
  void loop() {
    drainLoopTicks_.fetch_add(1, std::memory_order_relaxed);
    if (drainMutex == nullptr) return;
    if (xSemaphoreTake(drainMutex, 0) != pdTRUE) {
      drainMutexMisses_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    // Catch-all timing net: drain_ticks has repeatedly frozen at 0 for many
    // consecutive seconds, meaning this whole loop() call never returns -
    // applyPendingFormatChange()/drain() already have their own finer-
    // grained timers around each real I2S call, but this covers the
    // possibility that the actual stall is somewhere else in this function
    // entirely (e.g. inside AudioOutput's own bookkeeping). Remove once
    // root-caused.
    const uint32_t loopBodyStartUs = micros();
    pcmOutput.applyPendingFormatChange();
    if (isPlaying && !isPausedFlag) {
      pcmOutput.drain();
    } else {
      drainSkippedNotPlaying_.fetch_add(1, std::memory_order_relaxed);
    }
    const uint32_t loopBodyUs = micros() - loopBodyStartUs;
    if (loopBodyUs > 150000) {
      Serial.printf("AudioEngine::loop() body took %u us\n",
                    static_cast<unsigned>(loopBodyUs));
    }
    xSemaphoreGive(drainMutex);
  }

  // Dedicated decode task (Core 0, lower priority than AudioTask - see
  // begin()): repeatedly decode whichever buffer Core 1's read side most
  // recently finished filling. Blocks (with a short timeout, so
  // decodeEnabled_/EOF are still rechecked periodically even when idle)
  // instead of busy-polling, since this task no longer shares a loop
  // iteration with anything time-sensitive.
  static void decodeTaskTrampoline(void* param) {
    static_cast<AudioEngine*>(param)->decodeTaskLoop();
  }

  void decodeTaskLoop() {
    for (;;) {
      if (!decodeEnabled_.load(std::memory_order_acquire)) {
        vTaskDelay(pdMS_TO_TICKS(20));
        continue;
      }
      decodeConsumeStep(pdMS_TO_TICKS(20));
    }
  }

  // Decode whichever buffer Core 1's read side most recently finished
  // filling, if any is ready within waitTicks.
  void decodeConsumeStep(TickType_t waitTicks = 0) {
    xSemaphoreTake(decodeLifecycleMutex_, portMAX_DELAY);
    if (!decodeEnabled_.load(std::memory_order_acquire)) {
      xSemaphoreGive(decodeLifecycleMutex_);
      return;
    }
    if (xSemaphoreTake(bufferReadySem_, waitTicks) != pdTRUE) {
      // Nothing buffered right now. If the read side has already hit EOF
      // and there is nothing left queued (which this empty take() just
      // confirmed), every byte of the file has now been decoded - signal
      // decodeStep() (Core 1) that it is finally safe to stop playback.
      if (readSideEof_.load(std::memory_order_acquire)) {
        decodeSideEof_.store(true, std::memory_order_release);
      }
      xSemaphoreGive(decodeLifecycleMutex_);
      return;
    }
    if (!decodeEnabled_.load(std::memory_order_acquire)) {
      xSemaphoreGive(bufferFreeSem_);
      xSemaphoreGive(decodeLifecycleMutex_);
      return;
    }
    const int index = decodeIndex_;
    decodeIndex_ = 1 - decodeIndex_;
    const size_t len = readBufferLen_[index];
    const uint32_t decodeStartUs = micros();
    // decoderStream.write() ultimately reaches PcmRingOutput::write(),
    // which always either fully accepts len or blocks/drops after
    // MaxWriteWaitMs - it never returns a short count to retry, so a
    // single call here always fully consumes len.
    decoderStream.write(readBuffers_[index], len);
    const uint32_t decodeDurationUs = micros() - decodeStartUs;
    uint32_t prevMax = perfMaxDecodeUs.load(std::memory_order_relaxed);
    while (decodeDurationUs > prevMax &&
           !perfMaxDecodeUs.compare_exchange_weak(
               prevMax, decodeDurationUs, std::memory_order_relaxed)) {
    }
    xSemaphoreGive(bufferFreeSem_);
    xSemaphoreGive(decodeLifecycleMutex_);
  }

  // Core 1 (main.cpp's loop(), called between touch/UI work): read one
  // batch of encoded data per call and hand it to Core 0's decode step via
  // the double-buffered pipeline above. Running on the UI core - rather
  // than a dedicated task - means the SD read naturally yields to touch
  // handling and redraws, and only ever contends with
  // playPath()/stop()/pauseResume(), which also run on this same
  // core/thread.
  void decodeStep() {
    if (audioMutex != nullptr) {
      xSemaphoreTake(audioMutex, portMAX_DELAY);
    }
    applyPendingVolume();
    // Only Core 0 can know decode has truly caught up with EOF (see
    // decodeConsumeStep()), so this is the first place it is safe to
    // actually stop playback - doing it here, before the read-ahead check
    // below, means a track's very last buffered chunk is never dropped or
    // played twice.
    if (decodeSideEof_.load(std::memory_order_acquire)) {
      decodeSideEof_.store(false, std::memory_order_relaxed);
      // Diagnostic for the MP3/AAC "too fast" investigation: compare real
      // wall-clock playback duration against what the file's own size
      // implies it should take, to get an exact, unambiguous speed ratio
      // rather than guessing from bitrate assumptions. Remove once
      // root-caused.
      const uint32_t elapsedMs = millis() - trackStartMs_;
      Serial.printf(
          "Audio stream reached EOF: elapsed_ms=%lu file_bytes=%lu\n",
          static_cast<unsigned long>(elapsedMs),
          static_cast<unsigned long>(audioFile ? audioFile.fileSize() : 0));
      bitDepthUpconverter.logDiagnostics();
      trackFinished = true;
      stopUnlocked();
    }
    // Only read ahead while the ring has a healthy margin of free space,
    // and while the pipeline has a free buffer slot for decode to still be
    // working through. Without the ring check, decode (much faster than
    // real-time for compressed formats) would fill the ring almost
    // immediately, and every subsequent decoded chunk would have nowhere
    // to go - previously silently dropped, which sounded like tracks
    // racing through their content far too quickly. Skipping the read here
    // (cheap - just size checks/a non-blocking semaphore take) instead lets
    // touch/UI handling continue promptly and retries next iteration once
    // drain()/decode have freed enough room.
    const bool ringHasRoom =
        pcmOutput.availableForWrite() >= static_cast<int>(DecodeAheadMarginBytes);
    if (isPlaying && !isPausedFlag && ringHasRoom && audioFile &&
        audioFile.available() &&
        xSemaphoreTake(bufferFreeSem_, 0) == pdTRUE) {
      const uint32_t copyStartUs = micros();
      // Only the SD read itself needs sharedSpiMutex - decode now runs
      // concurrently on Core 0 with no SPI access at all, so narrowing the
      // mutex to just this read (rather than wrapping read+decode, as the
      // old single-core version did) also shortens how long the display
      // can be blocked waiting for the bus.
      const uint32_t spiWaitStartUs = micros();
      if (sharedSpiMutex != nullptr) {
        xSemaphoreTake(sharedSpiMutex, portMAX_DELAY);
      }
      const uint32_t spiAcquiredUs = micros();
      const size_t bytesRead = audioFile.readBytes(readBuffers_[fillIndex_],
                                                    AudioCopyBufferSize);
      if (sharedSpiMutex != nullptr) {
        xSemaphoreGive(sharedSpiMutex);
      }
      const uint32_t readDoneUs = micros();
      const uint32_t spiWaitUs = spiAcquiredUs - spiWaitStartUs;
      const uint32_t readDurationUs = readDoneUs - spiAcquiredUs;

      if (bytesRead > 0) {
        readBufferLen_[fillIndex_] = bytesRead;
        fillIndex_ = 1 - fillIndex_;
        // Hands this buffer off to Core 0's decodeConsumeStep().
        xSemaphoreGive(bufferReadySem_);
        perfBytes += bytesRead;
        ++perfCopies;
        if (spiWaitUs > perfMaxSpiWaitUs) {
          perfMaxSpiWaitUs = spiWaitUs;
        }
        if (readDurationUs > perfMaxReadUs) {
          perfMaxReadUs = readDurationUs;
        }
      } else {
        // Nothing read (shouldn't normally happen since audioFile.available()
        // was already checked above) - return the slot immediately rather
        // than leaking it.
        xSemaphoreGive(bufferFreeSem_);
      }
      const uint32_t copyDurationUs = micros() - copyStartUs;
      if (bytesRead > 0 && copyDurationUs > perfMaxCopyUs) {
        perfMaxCopyUs = copyDurationUs;
      }
      if (!audioFile.available()) {
        // Only marks that the SD side is done - see decodeConsumeStep() for
        // why the actual stop is deferred to Core 0 catching up.
        readSideEof_.store(true, std::memory_order_release);
      }
    }
    if (isPlaying && EnablePeriodicPerfLogging) {
      const uint32_t nowMs = millis();
      if (nowMs - perfWindowStartMs >= 1000) {
        Serial.printf(
            "Audio perf: encoded_bytes/s=%lu copies/s=%lu max_copy_us=%lu "
            "max_spi_wait_us=%lu max_read_us=%lu max_decode_us=%lu "
            "drain_us=%lu pcm_queued=%u "
            "pcm_highwater=%u/%u pcm_consumed=%lu pcm_dropped=%lu "
            "pcm_overflows=%lu pcm_short_write=%lu pcm_zero_free=%lu "
            "pcm_stalled_calls=%lu drain_ticks=%lu drain_mutex_miss=%lu "
            "drain_skip_not_playing=%lu\n",
                      static_cast<unsigned long>(perfBytes),
                      static_cast<unsigned long>(perfCopies),
                      static_cast<unsigned long>(perfMaxCopyUs),
                      static_cast<unsigned long>(perfMaxSpiWaitUs),
                      static_cast<unsigned long>(perfMaxReadUs),
                      static_cast<unsigned long>(
                          perfMaxDecodeUs.load(std::memory_order_relaxed)),
                      static_cast<unsigned long>(pcmOutput.drainUs()),
                      static_cast<unsigned>(pcmOutput.queuedBytes()),
                      static_cast<unsigned>(pcmOutput.highWaterBytes()),
                      static_cast<unsigned>(pcmOutput.capacityBytes()),
                      static_cast<unsigned long>(pcmOutput.consumedBytes()),
                      static_cast<unsigned long>(pcmOutput.droppedBytes()),
                      static_cast<unsigned long>(pcmOutput.overflowCount()),
                      static_cast<unsigned long>(pcmOutput.shortWriteBytes()),
                      static_cast<unsigned long>(pcmOutput.zeroFreeBreaks()),
                      static_cast<unsigned long>(pcmOutput.stalledCalls()),
                      static_cast<unsigned long>(
                          drainLoopTicks_.exchange(0, std::memory_order_relaxed)),
                      static_cast<unsigned long>(
                          drainMutexMisses_.exchange(0, std::memory_order_relaxed)),
                      static_cast<unsigned long>(drainSkippedNotPlaying_.exchange(
                          0, std::memory_order_relaxed)));
        perfWindowStartMs = nowMs;
        perfBytes = 0;
        perfCopies = 0;
        perfMaxCopyUs = 0;
        perfMaxSpiWaitUs = 0;
        perfMaxReadUs = 0;
        perfMaxDecodeUs.store(0, std::memory_order_relaxed);
        pcmOutput.resetPerfWindow();
      }
    }
    if (audioMutex != nullptr) {
      xSemaphoreGive(audioMutex);
    }
  }

  void stopUnlocked() {
    isPlaying = false;
    isPausedFlag = false;
    // Unblock writes waiting on a now-undrained ring before waiting for
    // exclusive decoder ownership. Never free buffers after a timeout
    // while decode still uses them.
    decodeEnabled_.store(false, std::memory_order_release);
    pcmOutput.cancelWrites();
    const uint32_t stopStartMs = millis();
    if (decodeLifecycleMutex_ != nullptr) {
      xSemaphoreTake(decodeLifecycleMutex_, portMAX_DELAY);
    }
    // Discard any buffers Core 1 finished reading but Core 0 hadn't yet
    // decoded - fine to drop since playback is stopping/changing tracks
    // anyway - and return their slots so bufferFreeSem_ is back at its full
    // starting count for the next playPath().
    while (xSemaphoreTake(bufferReadySem_, 0) == pdTRUE) {
      xSemaphoreGive(bufferFreeSem_);
    }
    readSideEof_.store(false, std::memory_order_relaxed);
    decodeSideEof_.store(false, std::memory_order_relaxed);
    fillIndex_ = 0;
    decodeIndex_ = 0;
    volumeOut.setVolume(0.0f);
    delay(5);
    decoderStream.end();
    // isPlaying is now false, so loop()'s drain() call is gated off; take
    // drainMutex to also exclude its (unconditional) applyPendingFormatChange
    // check while flush() resets pcmRing_/output_.
    if (drainMutex != nullptr) {
      xSemaphoreTake(drainMutex, portMAX_DELAY);
    }
    pcmOutput.flush();
    if (drainMutex != nullptr) {
      xSemaphoreGive(drainMutex);
    }
    volumeOut.setVolume(getRequestedVolume());
    if (audioFile) {
      audioFile.close();
    }
    if (temporaryPath.length() > 0) {
      extern SdFat sd;
      sd.remove(temporaryPath.c_str());
      temporaryPath = "";
    }
    if (decodeLifecycleMutex_ != nullptr) {
      xSemaphoreGive(decodeLifecycleMutex_);
    }
    Serial.printf("Audio stop complete: elapsed_ms=%lu\n",
                  static_cast<unsigned long>(millis() - stopStartMs));
  }

  void stop() {
    if (audioMutex != nullptr) {
      xSemaphoreTake(audioMutex, portMAX_DELAY);
    }
    stopUnlocked();
    trackFinished = false;
    if (audioMutex != nullptr) {
      xSemaphoreGive(audioMutex);
    }
  }

  void stopSong() {
    stop();
  }

  bool consumeTrackFinished() {
    if (audioMutex != nullptr &&
        xSemaphoreTake(audioMutex, 0) != pdTRUE) {
      return false;
    }
    const bool finished = trackFinished;
    trackFinished = false;
    if (audioMutex != nullptr) {
      xSemaphoreGive(audioMutex);
    }
    return finished;
  }

  void setVolume(float volumePercent) {
    if (!isfinite(volumePercent) || volumePercent < 0.0f) {
      Serial.printf("Ignoring invalid volume: %f\n", volumePercent);
      return;
    }
    volumePercent = min(volumePercent, 1.0f);
    portENTER_CRITICAL(&volumeMux);
    requestedVolume = volumePercent;
    volumeUpdatePending = true;
    portEXIT_CRITICAL(&volumeMux);
  }

  void setVolume(uint8_t volume21Scale) {
    float percent = (float)volume21Scale / 21.0f;
    setVolume(percent);
  }

  void setVolume(int volumeInt) {
    float percent = volumeInt > 21 ? (float)volumeInt / 100.0f : (float)volumeInt / 21.0f;
    setVolume(percent);
  }

  void pauseResume() {
    if (!isPlaying) return;
    isPausedFlag = !isPausedFlag;
  }

  bool isActive() {
    return isPlaying && !isPausedFlag;
  }

  uint32_t getAudioCurrentTime() {
    if (audioFile && audioFile.fileSize() > 0) {
      return (uint32_t)(audioFile.curPosition() / 1000); 
    }
    return 0;
  }

  void setAudioPlayPosition(uint32_t pos) {
    if (audioFile) {
      audioFile.seekSet(pos * 1000);
    }
  }
};
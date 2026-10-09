Import("env")

from pathlib import Path


def patch_multi_decoder():
    project_dir = Path(env.subst("$PROJECT_DIR"))
    decoder_path = (
        project_dir
        / ".pio"
        / "libdeps"
        / env.subst("$PIOENV")
        / "audio-tools"
        / "src"
        / "AudioTools"
        / "AudioCodecs"
        / "MultiDecoder.h"
    )
    if not decoder_path.exists():
        return

    source = decoder_path.read_text()
    old = """    if (StrView(mime).equalsIgnoreCase(actual_decoder.mime)) {
      is_first = false;
      if (actual_decoder.decoder != nullptr) {
        if (actual_decoder.is_open) actual_decoder.decoder->end();
        actual_decoder.decoder->begin();
        actual_decoder.is_open = true;
      }
      return true;
    }"""
    new = """    if (StrView(mime).equalsIgnoreCase(actual_decoder.mime)) {
      // Container decoders can select the same MIME once per encoded frame.
      // Keep an active codec open so its frame state is not discarded.
      is_first = false;
      return true;
    }"""
    if old in source:
        decoder_path.write_text(source.replace(old, new))


def patch_m4a_reset():
    project_dir = Path(env.subst("$PROJECT_DIR"))
    demuxer_path = (
        project_dir
        / ".pio"
        / "libdeps"
        / env.subst("$PIOENV")
        / "audio-tools"
        / "src"
        / "AudioTools"
        / "AudioCodecs"
        / "M4ACommonDemuxer.h"
    )
    if demuxer_path.exists():
        source = demuxer_path.read_text()
        old = """      box_size = 0;
    }

    /**
     * @brief Sets the callback"""
        new = """      box_size = 0;
      fixed_sample_size = 0;
      fixed_sample_count = 0;
    }

    /**
     * @brief Sets the callback"""
        if old in source and "      box_size = 0;\n      fixed_sample_size = 0;" not in source:
            demuxer_path.write_text(source.replace(old, new, 1))


def patch_m4a_demuxer_begin():
    project_dir = Path(env.subst("$PROJECT_DIR"))
    demuxer_path = (
        project_dir
        / ".pio"
        / "libdeps"
        / env.subst("$PIOENV")
        / "audio-tools"
        / "src"
        / "AudioTools"
        / "AudioCodecs"
        / "M4AAudioDemuxer.h"
    )
    if demuxer_path.exists():
        source = demuxer_path.read_text()
        old = """    stsz_processed = false;
    stco_processed = false;

    // When codec/sampleSizes/callback/ref change, update the extractor:"""
        new = """    stsz_processed = false;
    stco_processed = false;
    stsd_processed = false;
    sample_count = 0;
    stsz_offset = 0;
    chunk_offsets_count = 0;

    // When codec/sampleSizes/callback/ref change, update the extractor:"""
        if old in source and "sample_count = 0;" not in source:
            demuxer_path.write_text(source.replace(old, new, 1))


def patch_foxen_block_logging():
    project_dir = Path(env.subst("$PROJECT_DIR"))
    decoder_path = (
        project_dir
        / ".pio"
        / "libdeps"
        / env.subst("$PIOENV")
        / "audio-tools"
        / "src"
        / "AudioTools"
        / "AudioCodecs"
        / "CodecFLACFoxen.h"
    )
    if not decoder_path.exists():
        return

    source = decoder_path.read_text()
    old = '    LOGI("writeBlocking: %d", out_len * sizeof(int16_t));\n'
    if old in source:
        decoder_path.write_text(source.replace(old, "", 1))


def patch_foxen_bit_depth_conversion():
    project_dir = Path(env.subst("$PROJECT_DIR"))
    decoder_path = (
        project_dir
        / ".pio"
        / "libdeps"
        / env.subst("$PIOENV")
        / "audio-tools"
        / "src"
        / "AudioTools"
        / "AudioCodecs"
        / "CodecFLACFoxen.h"
    )
    if not decoder_path.exists():
        return

    source = decoder_path.read_text()
    old = """    const int shift = bits_eff > 16 ? bits_eff - 16 : 0;
    for (int j = 0; j < out_len; j++) {
      out16[j] = static_cast<int16_t>(out.data()[j] >> shift);
    }"""
    new = """    for (int j = 0; j < out_len; j++) {
      // Foxen left-aligns every source bit depth in signed 32-bit output.
      out16[j] = static_cast<int16_t>(out.data()[j] >> 16);
    }"""
    if old in source:
        decoder_path.write_text(source.replace(old, new, 1))


def patch_foxen_native_output_info():
    project_dir = Path(env.subst("$PROJECT_DIR"))
    decoder_path = (
        project_dir
        / ".pio"
        / "libdeps"
        / env.subst("$PIOENV")
        / "audio-tools"
        / "src"
        / "AudioTools"
        / "AudioCodecs"
        / "CodecFLACFoxen.h"
    )
    if not decoder_path.exists():
        return

    source = decoder_path.read_text()
    old = "    info.bits_per_sample = is_convert_to_16 ? 16 : bits_eff;"
    new = (
        "    // Native Foxen output is left-aligned int32_t, so transport it "
        "in 32-bit I2S slots even for 24-bit source files.\n"
        "    info.bits_per_sample = is_convert_to_16 ? 16 : 32;"
    )
    if old in source:
        decoder_path.write_text(source.replace(old, new, 1))


def patch_aac_sbr_diagnostic():
    # Diagnostic for the MP3/AAC "too fast" investigation: CodecAACHelix.h's
    # own info-change log only ever prints i.sampRateOut (e.g. "44100"),
    # which is ambiguous - _AACFrameInfo::sampRateOut is defined (see
    # libhelix's aacdec.c) as sampRateCore * (sbrEnabled ? 2 : 1), so a
    # printed 44100 could mean either "genuinely detected 44100, no SBR" or
    # "falsely/correctly detected SBR doubling a 22050 core rate" - both
    # look identical from sampRateOut alone. If the file's true bitstream
    # were actually SBR (implicit signaling is legal even when the
    # container's own AudioSpecificConfig says plain AAC-LC) but something
    # about M4A's raw-block decode path fails to apply the 2x upsampling
    # that should accompany a genuine sbrEnabled=1 core-rate mismatch, this
    # would explain an exact ~2x playback speed-up. Adding sampRateCore/
    # profile/outputSamps to the log makes this directly observable on a
    # hardware log instead of guessing from the ambiguous combined field.
    # Remove once root-caused.
    project_dir = Path(env.subst("$PROJECT_DIR"))
    decoder_path = (
        project_dir
        / ".pio"
        / "libdeps"
        / env.subst("$PIOENV")
        / "audio-tools"
        / "src"
        / "AudioTools"
        / "AudioCodecs"
        / "CodecAACHelix.h"
    )
    if not decoder_path.exists():
        return

    source = decoder_path.read_text()
    old = '      LOGW("sample_rate: %d / channels: %d", i.sampRateOut, i.nChans);'
    new = (
        '      LOGW("sample_rate: %d / channels: %d / core_rate: %d / '
        'profile: %d / outputSamps: %d / bitsPerSample: %d", i.sampRateOut, '
        "i.nChans, i.sampRateCore, i.profile, i.outputSamps, i.bitsPerSample);"
    )
    if old in source:
        decoder_path.write_text(source.replace(old, new, 1))


def patch_mp3_frame_diagnostic():
    decoder_path = (
        Path(env.subst("$PROJECT_DIR"))
        / ".pio"
        / "libdeps"
        / env.subst("$PIOENV")
        / "libhelix"
        / "src"
        / "MP3DecoderHelix.h"
    )
    if not decoder_path.exists():
        raise RuntimeError(f"MP3 diagnostic target missing: {decoder_path}")

    source = decoder_path.read_text()
    marker = "MP3 first frame diagnostic:"
    if marker in source:
        return
    old = """    if (info.outputSamps > 0) {
      // provide result"""
    new = """    if (info.outputSamps > 0) {
      if (mp3FrameInfo.samprate == 0) {
        Serial.printf(
            "MP3 first frame diagnostic: rate=%d channels=%d bits=%d "
            "output_samples=%d layer=%d version=%d bitrate=%d "
            "info_callback=%d pcm_callback=%d\\n",
            info.samprate, info.nChans, info.bitsPerSample, info.outputSamps,
            info.layer, info.version, info.bitrate,
            infoCallback != nullptr, pcmCallback != nullptr);
      }
      // provide result"""
    if old not in source:
        raise RuntimeError(f"MP3 diagnostic patch no longer matches: {decoder_path}")
    decoder_path.write_text(source.replace(old, new, 1))


def patch_helix_resync_diagnostic():
    # Diagnostic for the MP3/AAC "too fast" investigation: every per-frame
    # decode byte-accounting and bit-depth theory has been ruled out, yet
    # total decoded audio consistently comes out to ~half the file's real
    # duration while 100% of the file's compressed bytes get consumed by
    # EOF. CommonHelix::writeChunk()'s loop only has one way to consume
    # bytes *without* producing a frame of PCM: resynch()'s "invalid data"
    # paths (rc==0 after repeated stalls, or rc<-1) call
    # removeInvalidData(pos), which discards pos bytes as garbage and never
    # calls provideResult()/writeToOut(). If roughly every other real frame
    # is being misdetected as invalid here (e.g. a false/missed sync match)
    # that would silently swallow ~half the audio while still advancing
    # past its compressed bytes - exactly matching every measurement so
    # far. This patch counts real decode successes (rc>0) vs. bytes
    # discarded as invalid, and prints a running tally periodically so a
    # hardware log can show directly whether this path is firing anywhere
    # near as often as the ~2x ratio implies. Remove once root-caused.
    project_dir = Path(env.subst("$PROJECT_DIR"))
    common_path = (
        project_dir
        / ".pio"
        / "libdeps"
        / env.subst("$PIOENV")
        / "libhelix"
        / "src"
        / "CommonHelix.h"
    )
    if not common_path.exists():
        return

    source = common_path.read_text()
    old = """  virtual size_t writeChunk(const void *in_ptr, size_t in_size) {
    LOGI_HELIX("writeChunk %zu", in_size);
#ifdef ARDUINO
    time_last_write = millis();
#endif
    size_t result = frame_buffer.writeArray((uint8_t *)in_ptr, in_size);

    while (frame_buffer.available() >= minFrameBufferSize()) {
      if (!presync()) break;
      int rc = decode();
      if (!resynch(rc)) break;

      LOGI_HELIX("rc: %d - available %d", rc, frame_buffer.available());
    }

    return result;
  }"""
    new = """  virtual size_t writeChunk(const void *in_ptr, size_t in_size) {
    LOGI_HELIX("writeChunk %zu", in_size);
#ifdef ARDUINO
    time_last_write = millis();
#endif
    size_t result = frame_buffer.writeArray((uint8_t *)in_ptr, in_size);

    while (frame_buffer.available() >= minFrameBufferSize()) {
      if (!presync()) break;
      int rc = decode();
      int available_before = frame_buffer.available();
      if (rc > 0) {
        helixDiagFrameOk_++;
      } else {
        helixDiagFrameBad_++;
      }
      if (!resynch(rc)) break;
      int discarded = available_before - frame_buffer.available();
      if (rc <= 0 && discarded > 0) {
        helixDiagDiscardedBytes_ += discarded;
      }
      if ((helixDiagFrameOk_ + helixDiagFrameBad_) % 500 == 0) {
        Serial.printf(
            "Helix resync diagnostic: frames_ok=%lu frames_bad=%lu "
            "discarded_bytes=%lu\\n",
            (unsigned long)helixDiagFrameOk_, (unsigned long)helixDiagFrameBad_,
            (unsigned long)helixDiagDiscardedBytes_);
      }

      LOGI_HELIX("rc: %d - available %d", rc, frame_buffer.available());
    }

    return result;
  }

  unsigned long helixDiagFrameOk_ = 0;
  unsigned long helixDiagFrameBad_ = 0;
  unsigned long helixDiagDiscardedBytes_ = 0;"""
    if old in source:
        common_path.write_text(source.replace(old, new, 1))


def patch_foxen_psram_buffers():
    project_dir = Path(env.subst("$PROJECT_DIR"))
    decoder_path = (
        project_dir
        / ".pio"
        / "libdeps"
        / env.subst("$PIOENV")
        / "audio-tools"
        / "src"
        / "AudioTools"
        / "AudioCodecs"
        / "CodecFLACFoxen.h"
    )
    if not decoder_path.exists():
        return

    source = decoder_path.read_text()
    # All three of Foxen's internal buffers are touched inside the decode
    # hot loop: foxen_data backs libfoxenflac's per-channel `blkbuf`
    # reconstruction arrays (read-modify-written on every decoded sample by
    # the LPC/fixed predictor, up to a 32-tap history lookup per sample),
    # write_buffer holds compressed input bytes read bit-by-bit by the
    # bitstream reader during entropy decoding, and out is written one
    # sample at a time as the decoder produces PCM. PSRAM's higher
    # random-access latency costs the most on exactly this kind of small,
    # frequent, latency-bound access pattern, and the cost scales with
    # sample rate (a 96kHz stream runs this loop ~2.2x more often per second
    # than 44.1kHz) - which lines up with 24-bit/96kHz FLAC specifically
    # being the unstable case even after MP3/16-bit FLAC were fixed.
    # Measured internal RAM headroom (200+ KiB free/largest-contiguous
    # before these allocations) comfortably fits all three buffers
    # (~8+64+64 KiB) in internal RAM, so none of them need PSRAM: reserve
    # PSRAM for the genuinely bulk/sequential buffers instead (the 1 MiB PCM
    # ring, StreamCopy's staging buffer).
    pristine = """  SingleBuffer<uint8_t> write_buffer{0};
  Vector<int32_t> out;
  Vector<uint8_t> foxen_data{0};"""
    # Two earlier revisions of this patch routed some/all of these buffers
    # through PSRAM; recognize both already-applied forms too so re-running
    # this script against a previously patched checkout still converges on
    # the pristine (all-internal-RAM) target instead of being a no-op.
    all_psram = """  AllocatorPSRAM foxenAllocator;
  SingleBuffer<uint8_t> write_buffer{0, foxenAllocator};
  Vector<int32_t> out{0, foxenAllocator};
  Vector<uint8_t> foxen_data{0, foxenAllocator};"""
    partial_psram = """  AllocatorPSRAM foxenAllocator;
  SingleBuffer<uint8_t> write_buffer{0, foxenAllocator};
  Vector<int32_t> out{0, foxenAllocator};
  Vector<uint8_t> foxen_data{0};"""
    if all_psram in source:
        decoder_path.write_text(source.replace(all_psram, pristine, 1))
    elif partial_psram in source:
        decoder_path.write_text(source.replace(partial_psram, pristine, 1))
    # else: already pristine (fresh install), nothing to do.


patch_multi_decoder()
patch_m4a_reset()
patch_m4a_demuxer_begin()
patch_foxen_block_logging()
patch_foxen_bit_depth_conversion()
patch_foxen_native_output_info()
patch_aac_sbr_diagnostic()
patch_mp3_frame_diagnostic()
patch_helix_resync_diagnostic()
patch_foxen_psram_buffers()

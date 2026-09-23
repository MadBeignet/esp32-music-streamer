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


patch_multi_decoder()
patch_m4a_reset()
patch_m4a_demuxer_begin()

#pragma once

#include <AudioTools.h>
#include <AudioTools/AudioCodecs/AudioEncoded.h>
#include <AudioTools/AudioCodecs/CodecAACHelix.h>
#include <AudioTools/AudioCodecs/CodecFLACFoxen.h>
#include <AudioTools/AudioCodecs/ContainerM4A.h>
#include <AudioTools/AudioCodecs/MultiDecoder.h>
#include <SdFat.h>
#include "PinConfig.h"

extern void audio_metadata(audio_tools::MetaDataType type, const char* value,
                           int length);

class AudioEngine {
private:
  static constexpr size_t AudioCopyBufferSize = 32 * 1024;
  SemaphoreHandle_t audioMutex = nullptr;
  audio_tools::AllocatorPSRAM psramAllocator;
  I2SStream i2s;
  VolumeStream volumeOut{i2s};
  FLACDecoderFoxen flacDecoder{32 * 1024, 2};
  AACDecoderHelix aacDecoder;
  MultiDecoder m4aDecoder;
  ContainerM4A m4aContainer{m4aDecoder};
  MultiDecoder decoder;
  EncodedAudioStream decoderStream{&volumeOut, &decoder};
  audio_tools::MetaDataID3 id3Metadata;
  StreamCopy copier{AudioCopyBufferSize, psramAllocator};
  FsFile audioFile;
  String temporaryPath;
  bool isPlaying = false;
  bool isPausedFlag = false;
  bool trackFinished = false;
  float outputVolume = 1.0f;

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
    Serial.printf("Audio memory before init: internal=%u, psram=%u\n",
                  ESP.getFreeHeap(), ESP.getFreePsram());
    audioMutex = xSemaphoreCreateMutex();
    if (audioMutex == nullptr) {
      Serial.println("Audio mutex allocation failed!");
      return;
    }
    auto cfg = i2s.defaultConfig(TX_MODE);
    cfg.sample_rate = 44100;
    cfg.channels = 2;
    // All registered decoders feed a common 16-bit PCM/I2S sink. Foxen
    // down-converts higher-resolution FLAC samples before writing here.
    cfg.bits_per_sample = 16;
    cfg.pin_bck = bclk;
    cfg.pin_ws = lrc;
    cfg.pin_data = dout;
    cfg.buffer_size = 1024;
    cfg.buffer_count = 20;
    if (!i2s.begin(cfg)) {
      Serial.println("I2S initialization failed!");
      return;
    }
    volumeOut.begin();
    id3Metadata.setCallback(audio_metadata);
    id3Metadata.setFilter(audio_tools::SELECT_ID3);
    id3Metadata.begin();
    flacDecoder.set32Bit(false);
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
    decoder.addDecoder(flacDecoder, "audio/flac");
    decoder.addDecoder(m4aContainer, "audio/mp4");
    decoder.addDecoder(m4aContainer, "audio/m4a");
    Serial.printf("Audio memory after init: internal=%u, psram=%u\n",
                  ESP.getFreeHeap(), ESP.getFreePsram());
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

    if (hasM4AExtension(filepath) && !isFastStartM4A(audioFile)) {
      Serial.println("M4A is not fast-start; creating temporary SD copy...");
      if (!createFastStartCopy(filepath, audioFile)) {
        Serial.println("M4A rewrite failed; file cannot be played.");
        audioFile.close();
        return;
      }
    }
    audioFile.seekSet(0);
    id3Metadata.begin();
    uint8_t metadataBuffer[1024];
    const size_t metadataBytes = audioFile.read(
        metadataBuffer, sizeof(metadataBuffer));
    if (metadataBytes > 0) {
      id3Metadata.write(metadataBuffer, metadataBytes);
    }
    audioFile.seekSet(0);
    
    // FoxenFLACDecoder uses the write-based decoder interface. Keep the
    // encoded copy buffer bounded so SD reads do not consume task stack/heap.
    flacDecoder.setInBufferSize(AudioCopyBufferSize);
    flacDecoder.setOutBufferSize(32 * 1024);
    // ContainerM4A keeps sample-table storage across decoder lifetimes.
    // Reset it explicitly because automatic album continuation starts a new
    // file without reconstructing the decoder object.
    m4aContainer.getDemuxer().begin();
    decoderStream.setOutput(volumeOut);
    if (!decoderStream.begin()) {
      Serial.println("Failed to initialize FLAC decoder!");
      audioFile.close();
      return;
    }
    Serial.printf("Audio buffers allocated: internal=%u, psram=%u\n",
                  ESP.getFreeHeap(), ESP.getFreePsram());
    copier.begin(decoderStream, audioFile);
    copier.setCheckAvailableForWrite(false);
    copier.setCheckAvailable(false);
    Serial.printf("Audio started: %s (%lu bytes)\n", filepath,
                  static_cast<unsigned long>(audioFile.fileSize()));
    isPlaying = true;
    isPausedFlag = false;
  }

  void loop() {
    const bool locked = audioMutex == nullptr ||
                        xSemaphoreTake(audioMutex, portMAX_DELAY) == pdTRUE;
    if (locked && isPlaying && !isPausedFlag && audioFile) {
      bool reachedEof = false;
      for (uint8_t pass = 0; pass < 4 && audioFile.available(); ++pass) {
        if (copier.copy() == 0) {
          reachedEof = true;
          break;
        }
      }
      if ((reachedEof || !audioFile.available()) && !audioFile.available()) {
        Serial.println("Audio stream reached EOF");
        trackFinished = true;
        stopUnlocked();
      }
      if (audioMutex != nullptr) {
        xSemaphoreGive(audioMutex);
      }
    } else if (locked && audioMutex != nullptr) {
      xSemaphoreGive(audioMutex);
    }
  }

  void stopUnlocked() {
    isPlaying = false;
    isPausedFlag = false;
    volumeOut.setVolume(0.0f);
    delay(5);
    decoderStream.end();
    i2s.flush();
    volumeOut.setVolume(outputVolume);
    if (audioFile) {
      audioFile.close();
    }
    if (temporaryPath.length() > 0) {
      extern SdFat sd;
      sd.remove(temporaryPath.c_str());
      temporaryPath = "";
    }
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
    if (audioMutex != nullptr) {
      xSemaphoreTake(audioMutex, portMAX_DELAY);
    }
    volumeOut.setVolume(volumePercent);
    outputVolume = volumePercent;
    if (audioMutex != nullptr) {
      xSemaphoreGive(audioMutex);
    }
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
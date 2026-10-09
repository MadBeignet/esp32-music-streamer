"""Host regression checks for the firmware's PCM format converter."""

from pathlib import Path
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[1]
    source = (root / "src/AudioEngine.h").read_text()
    start = source.index("class BitDepthUpconverter final")
    end = source.index("\nclass AudioEngine {", start)
    converter = source[start:end]
    harness = r"""
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
using std::min;
struct SerialStub {
  template<typename... Args>
  void printf(const char*, Args...) {}
} Serial;
namespace audio_tools {
struct AudioInfo {
  int sample_rate = 44100;
  int channels = 2;
  int bits_per_sample = 16;
};
class AudioOutput {
public:
  virtual ~AudioOutput() = default;
  virtual size_t write(const uint8_t*, size_t) = 0;
  virtual int availableForWrite() { return 4096; }
  virtual void setAudioInfo(AudioInfo info) { info_ = info; }
  AudioInfo audioInfo() { return info_; }
private:
  AudioInfo info_;
};
}
"""
    tests = r"""
class CaptureOutput : public audio_tools::AudioOutput {
public:
  std::vector<uint8_t> bytes;
  size_t write(const uint8_t* data, size_t len) override {
    bytes.insert(bytes.end(), data, data + len);
    return len;
  }
};
int main() {
  CaptureOutput output;
  BitDepthUpconverter converter(output);
  // Seed even when the format equals AudioOutput's default.
  converter.resetDiagnostics();
  converter.setAudioInfo({44100, 2, 16});
  assert(output.audioInfo().bits_per_sample == 32);
  assert(converter.availableForWrite() == 2048);
  std::vector<int16_t> pcm(1200);
  for (size_t i = 0; i < pcm.size(); ++i)
    pcm[i] = i % 2 ? INT16_MIN : INT16_MAX;
  assert(converter.write(reinterpret_cast<const uint8_t*>(pcm.data()),
                         pcm.size() * sizeof(int16_t)) ==
         pcm.size() * sizeof(int16_t));
  assert(output.bytes.size() == pcm.size() * sizeof(int32_t));
  for (size_t i = 0; i < pcm.size(); ++i) {
    int32_t sample;
    std::memcpy(&sample, output.bytes.data() + i * sizeof(sample), sizeof(sample));
    assert(sample == static_cast<int32_t>(pcm[i]) * 65536);
  }
  // FLAC passes native 32-bit PCM through; then MP3/AAC widens again.
  converter.resetDiagnostics();
  converter.setAudioInfo({96000, 2, 32});
  output.bytes.clear();
  assert(converter.availableForWrite() == 4096);
  int32_t flac[] = {INT32_MIN, INT32_MAX};
  converter.write(reinterpret_cast<const uint8_t*>(flac), sizeof(flac));
  assert(output.bytes.size() == sizeof(flac));
  assert(std::memcmp(output.bytes.data(), flac, sizeof(flac)) == 0);
  converter.resetDiagnostics();
  converter.setAudioInfo({48000, 1, 16});
  output.bytes.clear();
  converter.write(reinterpret_cast<const uint8_t*>(pcm.data()), 4);
  assert(output.bytes.size() == 8);
  assert(output.audioInfo().sample_rate == 48000);
  assert(output.audioInfo().channels == 1);
  converter.resetDiagnostics();
  converter.setAudioInfo({44100, 2, 16});
  output.bytes.clear();
  converter.write(reinterpret_cast<const uint8_t*>(pcm.data()), 4);
  assert(output.bytes.size() == 8);
}
"""
    with tempfile.TemporaryDirectory(prefix="streamer-pcm-format-") as directory:
        cpp = Path(directory) / "test.cpp"
        binary = Path(directory) / "test"
        cpp.write_text(harness + converter + tests)
        subprocess.run(
            ["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
             "-fsanitize=undefined", str(cpp), "-o", str(binary)],
            check=True,
        )
        subprocess.run([str(binary)], check=True)
    print("PCM conversion checks passed: default format, sample values, "
          "batch boundaries, FLAC passthrough, and track/format transitions.")


if __name__ == "__main__":
    main()

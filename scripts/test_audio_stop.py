"""Host concurrency regression for the firmware decode/stop handoff."""

from pathlib import Path
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[1]
    source = (root / "src/AudioEngine.h").read_text()
    start = source.index("  void decodeConsumeStep(")
    end = source.index("\n  // Core 1", start)
    decode = source[start:end]
    start = source.index("  void stopUnlocked() {")
    end = source.index("    // Discard any buffers", start)
    stop = source[start:end]
    cancel_start = source.index("  void cancelWrites()")
    cancel_end = source.index("\n  size_t write(", cancel_start)
    cancellation = source[cancel_start:cancel_end]
    harness = r"""
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
using TickType_t = unsigned long;
constexpr int pdTRUE = 1;
constexpr TickType_t portMAX_DELAY = ~TickType_t{0};
struct Semaphore {
  std::mutex mutex;
  std::condition_variable changed;
  int count = 1;
};
using SemaphoreHandle_t = Semaphore*;
int xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticks) {
  std::unique_lock<std::mutex> lock(semaphore->mutex);
  if (ticks == portMAX_DELAY) {
    semaphore->changed.wait(lock, [&] { return semaphore->count > 0; });
  } else if (!semaphore->changed.wait_for(lock, std::chrono::milliseconds(ticks),
                                         [&] { return semaphore->count > 0; })) {
    return 0;
  }
  --semaphore->count;
  return pdTRUE;
}
void xSemaphoreGive(SemaphoreHandle_t semaphore) {
  std::lock_guard<std::mutex> lock(semaphore->mutex);
  ++semaphore->count;
  semaphore->changed.notify_one();
}
uint32_t micros() { return 1; }
uint32_t millis() { return 1; }
class CancellableOutput {
public:
"""
    harness += cancellation + r"""
  std::atomic<bool> writesCancelled_{false};
};
struct Decoder {
  CancellableOutput* output;
  std::atomic<bool> inWrite{false};
  std::atomic<bool> freed{false};
  void write(uint8_t*, size_t) {
    assert(!freed.load());
    inWrite.store(true);
    while (!output->writesCancelled_.load(std::memory_order_acquire))
      std::this_thread::yield();
    // Simulate final decoder-buffer access after write cancellation.
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    assert(!freed.load());
    inWrite.store(false);
  }
};
class Engine {
public:
  CancellableOutput pcmOutput;
  Decoder decoderStream{&pcmOutput};
  Semaphore lifecycle, ready, freeSlots;
  SemaphoreHandle_t decodeLifecycleMutex_ = &lifecycle;
  SemaphoreHandle_t bufferReadySem_ = &ready;
  SemaphoreHandle_t bufferFreeSem_ = &freeSlots;
  std::atomic<bool> decodeEnabled_{true}, readSideEof_{false}, decodeSideEof_{false};
  std::atomic<uint32_t> perfMaxDecodeUs{0};
  bool isPlaying = true, isPausedFlag = false;
  int decodeIndex_ = 0;
  uint8_t storage[2] = {};
  uint8_t* readBuffers_[2] = {storage, storage + 1};
  size_t readBufferLen_[2] = {1, 1};
"""
    harness += decode + stop + r"""
    (void)stopStartMs;
    assert(!decoderStream.inWrite.load());
    decoderStream.freed.store(true);
    xSemaphoreGive(decodeLifecycleMutex_);
  }
};
int main() {
  for (int i = 0; i < 100; ++i) {
    Engine engine;
    std::thread worker([&] { engine.decodeConsumeStep(20); });
    while (!engine.decoderStream.inWrite.load()) std::this_thread::yield();
    engine.stopUnlocked();
    worker.join();
    assert(engine.decoderStream.freed.load());
    // A worker entering after stop must not touch the freed decoder.
    engine.decodeConsumeStep(0);
  }
  Engine idle;
  idle.ready.count = 0;
  idle.decodeConsumeStep(0);
  idle.stopUnlocked();
}
"""
    with tempfile.TemporaryDirectory(prefix="streamer-audio-stop-") as directory:
        cpp = Path(directory) / "test.cpp"
        binary = Path(directory) / "test"
        cpp.write_text(harness)
        subprocess.run(
            ["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
             str(cpp), "-o", str(binary)],
            check=True,
        )
        subprocess.run([str(binary)], check=True, timeout=15)
    print("Audio stop checks passed: blocked decode cancellation, exclusive "
          "teardown, post-stop admission, and idle stop.")


if __name__ == "__main__":
    main()

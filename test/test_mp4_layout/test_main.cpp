#include <stdint.h>
#include <fstream>
#include <string.h>
#include <string>
#include <vector>

#include <unity.h>

static uint32_t readBe32(const uint8_t* data) {
  return (static_cast<uint32_t>(data[0]) << 24) |
         (static_cast<uint32_t>(data[1]) << 16) |
         (static_cast<uint32_t>(data[2]) << 8) |
         static_cast<uint32_t>(data[3]);
}

static void writeBe32(uint8_t* data, uint32_t value) {
  data[0] = value >> 24;
  data[1] = value >> 16;
  data[2] = value >> 8;
  data[3] = value;
}

static bool hasMoovBeforeMdat(const std::vector<uint8_t>& file) {
  bool sawMoov = false;
  size_t offset = 0;
  while (offset + 8 <= file.size()) {
    uint32_t size = readBe32(file.data() + offset);
    if (size < 8 || offset + size > file.size()) return false;
    const uint8_t* type = file.data() + offset + 4;
    if (memcmp(type, "moov", 4) == 0) sawMoov = true;
    if (memcmp(type, "mdat", 4) == 0) return sawMoov;
    offset += size;
  }
  return false;
}

static void test_fast_start_layout_is_detected(void) {
  const std::vector<uint8_t> file = {
      0, 0, 0, 12, 'f', 't', 'y', 'p', 0, 0, 0, 0,
      0, 0, 0, 12, 'm', 'o', 'o', 'v', 0, 0, 0, 0,
      0, 0, 0, 12, 'm', 'd', 'a', 't', 0, 0, 0, 0};
  TEST_ASSERT_TRUE(hasMoovBeforeMdat(file));
}

static void test_mdat_before_moov_is_rejected_for_streaming_path(void) {
  const std::vector<uint8_t> file = {
      0, 0, 0, 12, 'f', 't', 'y', 'p', 0, 0, 0, 0,
      0, 0, 0, 12, 'm', 'd', 'a', 't', 0, 0, 0, 0,
      0, 0, 0, 12, 'm', 'o', 'o', 'v', 0, 0, 0, 0};
  TEST_ASSERT_FALSE(hasMoovBeforeMdat(file));
}

static void test_chunk_offset_adjustment_math(void) {
  uint8_t offset[4] = {0, 0, 3, 232};
  const int64_t delta = 4096;
  const int64_t adjusted = static_cast<int64_t>(readBe32(offset)) + delta;
  writeBe32(offset, static_cast<uint32_t>(adjusted));
  TEST_ASSERT_EQUAL_UINT32(5096, readBe32(offset));
}

static void test_alac_fixture_is_classified_as_unsupported(void) {
  const char* path =
      "test/fixtures/audio/contenders/02 What Difference Does It Make_.m4a";
  std::ifstream input(path, std::ios::binary);
  TEST_ASSERT_TRUE(input.good());

  std::string contents((std::istreambuf_iterator<char>(input)),
                       std::istreambuf_iterator<char>());
  TEST_ASSERT_NOT_EQUAL(std::string::npos, contents.find("alac"));
  TEST_ASSERT_EQUAL(std::string::npos, contents.find("mp4a"));
}

static void test_change_fixture_has_supported_aac_layout(void) {
  const char* path = "test/fixtures/audio/contenders/09 Change.m4a";
  std::ifstream input(path, std::ios::binary);
  TEST_ASSERT_TRUE(input.good());

  std::string contents((std::istreambuf_iterator<char>(input)),
                       std::istreambuf_iterator<char>());
  std::vector<uint8_t> file(contents.begin(), contents.end());

  TEST_ASSERT_TRUE(hasMoovBeforeMdat(file));
  TEST_ASSERT_NOT_EQUAL(std::string::npos, contents.find("mp4a"));
  TEST_ASSERT_NOT_EQUAL(std::string::npos, contents.find("stsz"));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_fast_start_layout_is_detected);
  RUN_TEST(test_mdat_before_moov_is_rejected_for_streaming_path);
  RUN_TEST(test_chunk_offset_adjustment_math);
  RUN_TEST(test_alac_fixture_is_classified_as_unsupported);
  RUN_TEST(test_change_fixture_has_supported_aac_layout);
  return UNITY_END();
}

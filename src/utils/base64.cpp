#include "utils/base64.h"

namespace vxcore {

namespace {

// Standard base64 alphabet (RFC 4648)
constexpr char kBase64Chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Decode table: maps ASCII char to 6-bit value, -1 for invalid, -2 for padding
constexpr int8_t kDecodeTable[256] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,  // 0-15
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,  // 16-31
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 62, -1, -1, -1, 63,  // 32-47 (+, /)
    52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1, -1, -2, -1, -1,  // 48-63 (0-9, =)
    -1, 0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14,  // 64-79 (A-O)
    15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, -1,  // 80-95 (P-Z)
    -1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,  // 96-111 (a-o)
    41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1,  // 112-127 (p-z)
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,  // 128-143
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,  // 144-159
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,  // 160-175
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,  // 176-191
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,  // 192-207
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,  // 208-223
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,  // 224-239
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,  // 240-255
};

}  // namespace

std::string Base64Encode(const uint8_t* data, size_t size) {
  if (data == nullptr || size == 0) {
    return "";
  }

  // Calculate output size: 4 chars per 3 bytes, rounded up
  size_t output_size = ((size + 2) / 3) * 4;
  std::string result;
  result.reserve(output_size);

  size_t i = 0;
  while (i + 2 < size) {
    // Process 3 bytes at a time
    uint32_t triple = (static_cast<uint32_t>(data[i]) << 16) |
                      (static_cast<uint32_t>(data[i + 1]) << 8) |
                      static_cast<uint32_t>(data[i + 2]);

    result += kBase64Chars[(triple >> 18) & 0x3F];
    result += kBase64Chars[(triple >> 12) & 0x3F];
    result += kBase64Chars[(triple >> 6) & 0x3F];
    result += kBase64Chars[triple & 0x3F];
    i += 3;
  }

  // Handle remaining bytes
  if (i < size) {
    uint32_t triple = static_cast<uint32_t>(data[i]) << 16;
    if (i + 1 < size) {
      triple |= static_cast<uint32_t>(data[i + 1]) << 8;
    }

    result += kBase64Chars[(triple >> 18) & 0x3F];
    result += kBase64Chars[(triple >> 12) & 0x3F];

    if (i + 1 < size) {
      result += kBase64Chars[(triple >> 6) & 0x3F];
    } else {
      result += '=';
    }
    result += '=';
  }

  return result;
}

std::string Base64Encode(const std::vector<uint8_t>& data) {
  return Base64Encode(data.data(), data.size());
}

std::vector<uint8_t> Base64Decode(const std::string& encoded) {
  if (encoded.empty()) {
    return {};
  }

  // Count valid base64 characters and check format
  size_t valid_chars = 0;
  size_t padding = 0;
  for (char c : encoded) {
    int8_t val = kDecodeTable[static_cast<uint8_t>(c)];
    if (val >= 0) {
      ++valid_chars;
    } else if (val == -2) {  // padding '='
      ++padding;
    } else if (c != '\n' && c != '\r' && c != ' ' && c != '\t') {
      // Invalid character (not whitespace)
      return {};
    }
  }

  // Validate: total valid chars + padding must be multiple of 4
  if ((valid_chars + padding) % 4 != 0) {
    return {};
  }
  if (padding > 2) {
    return {};
  }

  // Calculate output size
  size_t output_size = (valid_chars + padding) / 4 * 3 - padding;
  std::vector<uint8_t> result;
  result.reserve(output_size);

  uint32_t buffer = 0;
  int bits_collected = 0;

  for (char c : encoded) {
    int8_t val = kDecodeTable[static_cast<uint8_t>(c)];
    if (val >= 0) {
      buffer = (buffer << 6) | static_cast<uint32_t>(val);
      bits_collected += 6;

      if (bits_collected >= 8) {
        bits_collected -= 8;
        result.push_back(static_cast<uint8_t>((buffer >> bits_collected) & 0xFF));
      }
    }
    // Skip whitespace and padding
  }

  return result;
}

bool IsValidBase64(const std::string& str) {
  if (str.empty()) {
    return true;  // Empty string is valid
  }

  size_t valid_chars = 0;
  size_t padding = 0;
  bool seen_padding = false;

  for (char c : str) {
    int8_t val = kDecodeTable[static_cast<uint8_t>(c)];
    if (val >= 0) {
      if (seen_padding) {
        return false;  // Data after padding
      }
      ++valid_chars;
    } else if (val == -2) {  // padding '='
      seen_padding = true;
      ++padding;
      if (padding > 2) {
        return false;
      }
    } else if (c != '\n' && c != '\r' && c != ' ' && c != '\t') {
      return false;  // Invalid character
    }
  }

  return (valid_chars + padding) % 4 == 0;
}

}  // namespace vxcore

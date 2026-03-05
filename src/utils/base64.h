#ifndef VXCORE_UTILS_BASE64_H_
#define VXCORE_UTILS_BASE64_H_

#include <cstdint>
#include <string>
#include <vector>

namespace vxcore {

// RFC 4648 compliant Base64 encoding/decoding.

// Encode binary data to base64 string.
std::string Base64Encode(const uint8_t* data, size_t size);
std::string Base64Encode(const std::vector<uint8_t>& data);

// Decode base64 string to binary data.
// Returns empty vector on invalid input.
std::vector<uint8_t> Base64Decode(const std::string& encoded);

// Check if a string is valid base64.
bool IsValidBase64(const std::string& str);

}  // namespace vxcore

#endif  // VXCORE_UTILS_BASE64_H_

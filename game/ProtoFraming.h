#pragma once

#include <cstdint>
#include <string>

namespace gameproto {

constexpr uint32_t kMaxFrameSize = 4 * 1024 * 1024;

bool EncodeFrame(const std::string &payload, std::string *out);
bool TryDecodeOneFrame(std::string *buffer, std::string *payload);

}  // namespace gameproto

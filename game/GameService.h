#pragma once

#include <string>

namespace gameproto {

bool HandleFrame(const std::string &request_payload, std::string *response_frame);

}  // namespace gameproto

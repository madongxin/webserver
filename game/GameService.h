#pragma once

/**
 * @file GameService.h
 * @brief 单帧业务入口：protobuf 反序列化 -> GameLogic -> 编码响应帧
 *
 * 由 GameTcpGateway::OnMessage 在拆出一帧 payload 后调用。
 * 不负责 TCP、不负责粘包；只处理「一整段 GameRequest 字节」。
 */

#include <string>

namespace gameproto {

/**
 * @param request_payload  已去掉 4 字节长度头的 GameRequest 序列化数据
 * @param response_frame   输出：[4 字节大端长度] + GameResponse 序列化
 * @return false 表示 ParseFromString 失败或 Serialize/Encode 失败
 */
bool HandleFrame(const std::string &request_payload, std::string *response_frame);

}  // namespace gameproto

#pragma once

/**
 * @file ProtoFraming.h
 * @brief 游戏 TCP 帧格式：长度前缀 + payload
 *
 *   [ uint32_t len 网络字节序大端 ][ len 字节的 protobuf 消息体 ]
 */

#include <cstdint>
#include <string>

namespace gameproto {

/** 单帧 payload 上限 4MB，防止恶意超大长度头 */
constexpr uint32_t kMaxFrameSize = 4 * 1024 * 1024;

enum class FrameDecodeResult {
    Complete,    // 取出一帧并消费缓冲
    Incomplete,  // 半包，保留数据等待后续字节
    Invalid      // 非法长度 / 超限，调用方应关闭连接
};

/** 将 protobuf 序列化结果加上 4 字节长度头，写入 *out */
bool EncodeFrame(const std::string &payload, std::string *out);

/**
 * 从 *buffer 头部尝试解析一帧。
 * Complete：payload 填好，已 erase 消费字节。
 * Incomplete：不消费。
 * Invalid：不消费完整帧（可保留或由调用方清空并关连）。
 */
FrameDecodeResult DecodeOneFrame(std::string *buffer, std::string *payload);

/** 兼容旧接口：仅 Complete 返回 true（Incomplete/Invalid 均 false） */
bool TryDecodeOneFrame(std::string *buffer, std::string *payload);

}  // namespace gameproto

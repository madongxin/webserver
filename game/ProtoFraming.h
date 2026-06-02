#pragma once

/**
 * @file ProtoFraming.h
 * @brief 游戏 TCP 帧格式：长度前缀 + payload
 *
 * 与 GameProtobufClient 约定一致：
 *
 *   [ uint32_t len 网络字节序大端 ][ len 字节的 protobuf 消息体 ]
 *
 * 请求体类型：game::GameRequest
 * 响应体类型：game::GameResponse
 *
 * TryDecodeOneFrame 从「连接级流缓冲」中尝试取出一帧；数据不足一帧时返回 false，不消费缓冲。
 */

#include <cstdint>
#include <string>

namespace gameproto {

/** 单帧 payload 上限 4MB，防止恶意超大长度头 */
constexpr uint32_t kMaxFrameSize = 4 * 1024 * 1024;

/** 将 protobuf 序列化结果加上 4 字节长度头，写入 *out */
bool EncodeFrame(const std::string &payload, std::string *out);

/**
 * 从 *buffer 头部尝试解析一帧。
 * @param buffer  入参兼出参：成功时 erase 已消费的字节（4 + len）
 * @param payload 输出：不含长度头的消息体
 * @return true 表示成功取出一帧；false 表示需要更多 TCP 数据或长度非法
 */
bool TryDecodeOneFrame(std::string *buffer, std::string *payload);

}  // namespace gameproto

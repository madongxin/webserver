#pragma once

/**
 * @file Buffer.h
 * @brief 应用层读写缓冲区（muduo 风格：prepend | readable | writable）
 *
 *   [0 .. read_index_)     prepend 区（可复用，减少 realloc）
 *   [read_index_ .. write_index_)  可读数据
 *   [write_index_ .. size)         可写尾部
 *
 * TcpConnection 读：Append 到 buffer；业务 RetrieveAllAsString 取走
 * TcpConnection 写：Send 装不下的进 send_buf_，EPOLLOUT 时 Retrieve
 */

#include <memory>
#include <vector>
#include <string>
#include <cstring>
#include "common.h"

static const int kPrePendIndex = 8;
static const int kInitalSize = 1024;

class Buffer {
public:
    DISALLOW_COPY_AND_MOVE(Buffer);

    Buffer();
    ~Buffer();

    char *begin();
    const char *begin() const;
    char *beginread();
    const char *beginread() const;
    char *beginwrite();
    const char *beginwrite() const;

    void Append(const char *message);
    void Append(const char *message, int len);
    void Append(const std::string &message);

    int readablebytes() const;
    int writablebytes() const;
    int prependablebytes() const;

    char *Peek();
    const char *Peek() const;
    std::string PeekAsString(int len);
    std::string PeekAllAsString();

    void Retrieve(int len);
    std::string RetrieveAsString(int len);
    void RetrieveAll();
    std::string RetrieveAllAsString();
    void RetrieveUtil(const char *end);
    std::string RetrieveUtilAsString(const char *end);

    void EnsureWritableBytes(int len);

private:
    std::vector<char> buffer_;
    int read_index_;
    int write_index_;
};

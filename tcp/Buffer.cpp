#include "Buffer.h"
#include <string>
#include <assert.h>
#include <cstring>
Buffer::Buffer()
    : buffer_(kInitalSize),
      read_index_(kPrePendIndex),
      write_index_(kPrePendIndex){}

Buffer::~Buffer(){}

char *Buffer::begin() { return &*buffer_.begin(); }
const char *Buffer::begin() const { return &*buffer_.begin(); }
char* Buffer::beginread() { return begin() + read_index_; } 
const char* Buffer::beginread() const { return begin() + read_index_; }
char* Buffer::beginwrite() { return begin() + write_index_; }
const char* Buffer::beginwrite() const { return begin() + write_index_; }

void Buffer::Append(const char* message) {
    Append(message, static_cast<int>(strlen(message)));
}

void Buffer::Append(const char* message, int len) {
    EnsureWritableBytes(len);
    std::copy(message, message + len, beginwrite());
    write_index_ += len;
}

void Buffer::Append(const std::string& message) {
    Append(message.data(), static_cast<int>(message.size())); 
}


int Buffer::readablebytes() const { return write_index_ - read_index_; }
int Buffer::writablebytes() const { return static_cast<int>(buffer_.size()) - write_index_; } 
int Buffer::prependablebytes() const { return read_index_; }

char *Buffer::Peek() { return beginread(); }
const char *Buffer::Peek() const { return beginread(); }

std::string Buffer::PeekAsString(int len){
    return std::string(beginread(), beginread() + len);
}

std::string Buffer::PeekAllAsString(){
    return std::string(beginread(), beginwrite());
}

void Buffer::Retrieve(int len){
    assert(len >= 0);
    assert(readablebytes() >= len);
    if (len < readablebytes()) {
        read_index_ += len;
    } else {
        // 恰好消费完全部可读字节（含 len==0）
        RetrieveAll();
    }
}

void Buffer::RetrieveAll(){
    write_index_ = kPrePendIndex;
    read_index_ = write_index_;
}

void Buffer::RetrieveUtil(const char *end){
    // 保证没有进入可写部分
    assert(beginwrite() >= end);
    read_index_ += static_cast<int>(end - beginread());
}

std::string Buffer::RetrieveAsString(int len){
    assert(read_index_ + len <= write_index_);

    // Peek* 已返回临时对象，勿再 std::move（避免 pessimizing-move）
    std::string ret = PeekAsString(len);
    Retrieve(len);
    return ret;
}

std::string Buffer::RetrieveUtilAsString(const char *end){
    assert(beginwrite() >= end);
    std::string ret = PeekAsString(static_cast<int>(end - beginread()));
    RetrieveUtil(end);
    return ret;
}

std::string Buffer::RetrieveAllAsString(){
    assert(readablebytes() > 0);
    std::string ret = PeekAllAsString();
    RetrieveAll();
    return ret;
}

void Buffer::EnsureWritableBytes(int len){
    if(writablebytes() >= len)
        return;
    if(writablebytes() + prependablebytes() >= kPrePendIndex + len){
        // 如果此时writable和prepenable的剩余空间超过写的长度，则先将已有数据复制到初始位置，
        // 将不断读导致的read_index_后移使前方没有利用的空间利用上。
        std::copy(beginread(), beginwrite(), begin() + kPrePendIndex);
        write_index_ = kPrePendIndex + readablebytes();
        read_index_ = kPrePendIndex;
    }else{
        buffer_.resize(write_index_ + len);
    }
}

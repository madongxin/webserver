#pragma once

#include <cstdlib>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

/**
 * 写缓冲三段。默认不接入 TcpConnection，GAMEMESH_SENDBUF_V2=1 时才切换。
 * A 已经开始写的半帧，不能改。
 * B 回包和 ENTER/LEAVE，先进先出。
 * C 一帧位置。第一个字节写出前可以被整帧换掉。
 * 写出顺序固定是 A、B、C。
 *
 * 和旧路径允许的差异：
 * - C 在写出第一个字节前被替换，旧的位置帧不会出现在输出里。
 * - ENTER/LEAVE 带了某个实体时，如果 C 里有这个实体，整帧 C 丢掉，不改 protobuf。
 */
class SendBufV2 {
public:
    struct Stamp {
        uint64_t entity_id = 0;
        uint64_t state_seq = 0;
    };
    using Callback = std::function<void(std::vector<Stamp>, bool committed)>;

    void SetCallback(Callback cb) { cb_ = std::move(cb); }

    void PushControl(const std::string &frame, const std::vector<uint64_t> &strip_entities) {
        if (!strip_entities.empty() && move_started_bytes_ == 0 && !move_.empty()) {
            for (uint64_t id : strip_entities) {
                for (const auto &st : move_stamps_) {
                    if (st.entity_id == id) {
                        if (cb_)
                            cb_(move_stamps_, false);
                        move_.clear();
                        move_stamps_.clear();
                        break;
                    }
                }
            }
        }
        control_.push_back(frame);
    }

    void SetMove(const std::string &frame, std::vector<Stamp> stamps) {
        if (move_started_bytes_ > 0)
            return;
        if (!move_.empty() && cb_)
            cb_(move_stamps_, false);
        move_ = frame;
        move_stamps_ = std::move(stamps);
    }

    std::string Take(bool include_move) {
        std::string out = partial_;
        partial_.clear();
        for (const auto &frame : control_)
            out.append(frame);
        control_.clear();
        if (include_move && !move_.empty()) {
            out.append(move_);
            if (cb_)
                cb_(move_stamps_, true);
            move_.clear();
            move_stamps_.clear();
            move_started_bytes_ = 0;
        }
        return out;
    }

    bool has_move() const { return !move_.empty(); }

    std::string TakeAll() { return Take(true); }

    struct MovePiece {
        std::string frame;
        std::vector<Stamp> stamps;
    };

    /** 把 C 取出来，不回调。已经开始写的半帧不取。 */
    MovePiece DetachMove() {
        MovePiece piece;
        if (move_started_bytes_ > 0)
            return piece;
        piece.frame.swap(move_);
        piece.stamps.swap(move_stamps_);
        return piece;
    }

    bool pending() const { return !partial_.empty() || !control_.empty() || !move_.empty(); }

private:
    std::string partial_;
    std::vector<std::string> control_;
    std::string move_;
    std::vector<Stamp> move_stamps_;
    int move_started_bytes_ = 0;
    Callback cb_;
};

inline bool SendBufV2Enabled() {
    const char *env = std::getenv("GAMEMESH_SENDBUF_V2");
    return env && env[0] == '1';
}

inline bool ReadBackpressureEnabled() {
    const char *env = std::getenv("GAMEMESH_READ_BACKPRESSURE");
    return env && env[0] == '1';
}

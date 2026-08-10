#pragma once

/**
 * @file AsyncMysqlGameDbRepository.h
 * @brief MySQL 实现的 GameDB：worker 池执行事务 + outbox 发布线程
 */

#include "IGameDbRepository.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

class AsyncMysqlGameDbRepository : public IGameDbRepository {
public:
    static AsyncMysqlGameDbRepository &Instance();

    void Start(int worker_count = 2) override;
    void Stop() override;
    bool started() const override { return started_; }

    void ClaimMailAttachmentsAsync(GameDbMailClaimRequest req, MailClaimDone done) override;

    /** 同步等待（供 MailService / 测试；勿在 Reactor IO 线程调用） */
    GameDbMailClaimResult ClaimMailAttachments(GameDbMailClaimRequest req) override;

    /** 测试：立刻跑一轮 outbox 发布 */
    int PublishOnceForTest(int limit = 100);

    /** 空=仅本地日志；nats://host:port 时尝试 PUB 后再 MarkPublished */
    void SetNatsUrl(std::string url);

private:
    AsyncMysqlGameDbRepository() = default;
    ~AsyncMysqlGameDbRepository();

    struct Job {
        GameDbMailClaimRequest req;
        MailClaimDone done;
    };

    void WorkerLoop();
    void PublisherLoop();
    void PublishBatch(int limit);
    GameDbMailClaimResult DoClaimMail(const GameDbMailClaimRequest &req);

    std::mutex life_mu_;
    bool started_ = false;
    std::atomic<bool> stop_{false};

    std::mutex q_mu_;
    std::condition_variable q_cv_;
    std::deque<Job> q_;
    std::vector<std::thread> workers_;

    std::thread publisher_;
    std::mutex pub_mu_;
    std::condition_variable pub_cv_;
};

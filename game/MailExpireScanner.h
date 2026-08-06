#pragma once

/**
 * @file MailExpireScanner.h
 * @brief 邮件过期定时扫描（挂到主 EventLoop::RunEvery）
 */

class EventLoop;

class MailExpireScanner {
public:
    static MailExpireScanner &Instance();

    /** 注册周期性 ScanExpire；interval_sec<=0 时使用 mail.cnf 配置 */
    void StartPeriodic(EventLoop *loop, double interval_sec = 0.0);

private:
    MailExpireScanner() = default;
    void OnTick();
};

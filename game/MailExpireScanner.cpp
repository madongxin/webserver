#include "MailExpireScanner.h"

#include "EventLoop.h"
#include "Logging.h"
#include "MailConfig.h"
#include "MailService.h"

MailExpireScanner &MailExpireScanner::Instance() {
    static MailExpireScanner g;
    return g;
}

void MailExpireScanner::StartPeriodic(EventLoop *loop, double interval_sec) {
    if (!loop)
        return;
    double iv = interval_sec;
    if (iv <= 0.0)
        iv = MailConfig::Instance().Values().expire_scan_interval_sec;
    if (iv <= 0.0)
        iv = 60.0;
    LOG_INFO << "MailExpireScanner: every " << iv << "s";
    loop->RunEvery(iv, [this]() { OnTick(); });
}

void MailExpireScanner::OnTick() {
    const int n = MailService::Instance().ScanExpire(500);
    if (n > 0)
        LOG_INFO << "MailExpireScanner: expired " << n << " mails";
}

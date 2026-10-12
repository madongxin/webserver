/**
 * 邮件状态机 / 过期判定单元测试（无 gtest，手工断言可执行文件）
 * 构建：随 server 同编译选项单独 add_executable，或：
 *   g++ -std=c++14 -Igame -Ibase test/mail_unit_test.cpp game/MailTypes.cpp -o build/test/mail_unit_test
 */

#include "MailTypes.h"

#include <cstdio>
#include <cstdlib>

static int g_fail = 0;

#define EXPECT_TRUE(cond)                                                                          \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
            ++g_fail;                                                                              \
        }                                                                                          \
    } while (0)

int main() {
    using namespace mail;

    EXPECT_TRUE(CanTransitVisible(VisibleState::kActive, VisibleState::kExpired));
    EXPECT_TRUE(CanTransitVisible(VisibleState::kActive, VisibleState::kSoftDeleted));
    EXPECT_TRUE(CanTransitVisible(VisibleState::kActive, VisibleState::kRevoked));
    EXPECT_TRUE(!CanTransitVisible(VisibleState::kExpired, VisibleState::kActive));
    EXPECT_TRUE(!CanTransitVisible(VisibleState::kSoftDeleted, VisibleState::kExpired));

    EXPECT_TRUE(CanTransitAttachment(AttachmentState::kUnclaimed, AttachmentState::kClaiming));
    EXPECT_TRUE(CanTransitAttachment(AttachmentState::kClaiming, AttachmentState::kClaimed));
    EXPECT_TRUE(CanTransitAttachment(AttachmentState::kClaiming, AttachmentState::kUnclaimed));
    EXPECT_TRUE(!CanTransitAttachment(AttachmentState::kClaimed, AttachmentState::kUnclaimed));
    EXPECT_TRUE(!CanTransitAttachment(AttachmentState::kNone, AttachmentState::kClaimed));

    EXPECT_TRUE(IsExpiredAt(100, 100));
    EXPECT_TRUE(IsExpiredAt(100, 101));
    EXPECT_TRUE(!IsExpiredAt(100, 99));
    EXPECT_TRUE(!IsExpiredAt(0, 999));
    EXPECT_TRUE(PreserveAttachmentOnExpire("UNCLAIMED"));
    EXPECT_TRUE(PreserveAttachmentOnExpire("CLAIMING"));
    EXPECT_TRUE(!PreserveAttachmentOnExpire("NONE"));
    EXPECT_TRUE(!PreserveAttachmentOnExpire("CLAIMED"));
    EXPECT_TRUE(OpIntentMatches(7, 9, "READ", 7, 9, "READ"));
    EXPECT_TRUE(!OpIntentMatches(7, 9, "READ", 8, 9, "READ"));
    EXPECT_TRUE(!OpIntentMatches(7, 9, "READ", 7, 10, "READ"));
    EXPECT_TRUE(!OpIntentMatches(7, 9, "READ", 7, 9, "CLAIM"));
    EXPECT_TRUE(AttachmentFingerprint("ITEM", 3, 2, "", "p") == "ITEM|3|2|NONE|p");
    EXPECT_TRUE(AttachmentFingerprint("ITEM", 3, 2, "NONE", "p") !=
                AttachmentFingerprint("ITEM", 4, 2, "NONE", "p"));

    Category c;
    EXPECT_TRUE(ParseCategory("ACTIVITY", &c) && c == Category::kActivity);
    EXPECT_TRUE(!ParseCategory("UNKNOWN", &c));

    if (g_fail) {
        std::fprintf(stderr, "%d assertion(s) failed\n", g_fail);
        return 1;
    }
    std::printf("mail_unit_test OK\n");
    return 0;
}

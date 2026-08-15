#include "Utf8Text.h"
#include "SocialText.h"

#include <cstdio>
#include <string>

namespace {

int fails = 0;
void Expect(bool c, const char *m) {
    if (!c) {
        std::printf("FAIL: %s\n", m);
        ++fails;
    }
}

}  // namespace

int main() {
    size_t n = 0;
    Expect(utf8text::CountCodePoints("abc", &n) && n == 3, "ascii");
    Expect(utf8text::CountCodePoints("玩家", &n) && n == 2, "cjk");
    Expect(!utf8text::CountCodePoints("\xff", &n), "bad utf8");
    std::string tec;
    Expect(utf8text::ValidBoundedText("hello", 1, 64, false, &tec), "title ok");
    Expect(!utf8text::ValidBoundedText("a\nb", 1, 64, false, &tec) && tec == "ERR_TEXT_CONTROL",
           "title newline");
    Expect(utf8text::ValidBoundedText("a\nb", 1, 1000, true, &tec), "body newline");
    Expect(!utf8text::ValidBoundedText("", 1, 64, false, &tec) && tec == "ERR_TEXT_LENGTH", "empty");
    const std::string long64(64, 'x');
    Expect(utf8text::ValidBoundedText(long64, 1, 64, false, &tec), "64 ok");
    Expect(!utf8text::ValidBoundedText(long64 + "y", 1, 64, false, &tec), "65 fail");
    std::string nm;
    Expect(social::NormalizePlayerName("  玩家甲  ", &nm, &tec) && nm == "玩家甲", "trim name");
    Expect(!social::NormalizePlayerName("   ", &nm, &tec), "blank name");
    Expect(social::ValidWorldChannel("") && social::ValidWorldChannel("world"), "world ch");
    Expect(!social::ValidWorldChannel("guild"), "no guild");
    Expect(social::ValidWorldChatText("hi", 200, 800, &tec), "chat ok");
    Expect(!social::ValidWorldChatText(std::string(801, 'x'), 2000, 800, &tec), "chat bytes");
    if (fails) {
        std::printf("utf8_text_test FAIL count=%d\n", fails);
        return 1;
    }
    std::printf("OK utf8_text_test\n");
    return 0;
}

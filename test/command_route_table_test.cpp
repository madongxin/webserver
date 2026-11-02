#include "CommandPolicy.h"
#include "CommandSpec.h"
#include "MessageRoute.h"

#include <cstdio>
#include <set>
#include <string>

namespace {

int g_fails = 0;

void Expect(bool ok, const std::string &msg) {
    if (ok)
        return;
    std::printf("FAIL: %s\n", msg.c_str());
    ++g_fails;
}

}  // namespace

int main() {
    const google::protobuf::OneofDescriptor *oneof =
        game::GameRequest::descriptor()->FindOneofByName("body");
    Expect(oneof != nullptr, "GameRequest has body oneof");
    if (!oneof)
        return 1;

    std::set<int> seen;
    int table_n = 0;
    const gameproto::CommandSpec *table = gameproto::CommandSpecTable(&table_n);
    for (int i = 0; i < table_n; ++i)
        seen.insert(static_cast<int>(table[i].body));

    int covered = 0;
    for (int i = 0; i < oneof->field_count(); ++i) {
        const google::protobuf::FieldDescriptor *field = oneof->field(i);
        const auto body = static_cast<game::GameRequest::BodyCase>(field->number());
        const gameproto::CommandSpec *spec = gameproto::FindCommandSpec(body);
        Expect(spec != nullptr, std::string("missing spec for ") + field->name());
        if (!spec)
            continue;
        ++covered;
        seen.erase(static_cast<int>(body));

        game::GameRequest req;
        const google::protobuf::Reflection *ref = req.GetReflection();
        ref->MutableMessage(&req, field);
        Expect(gameproto::IsMailBoundRequest(req) == spec->mail, field->name() + std::string(" mail"));
        Expect(gameproto::IsGlobalServiceRequest(req) == spec->global_service,
               field->name() + std::string(" route"));
        Expect(!gameproto::IsGlobalServiceRequest(req) == gameproto::IsLogicBoundRequest(req),
               field->name() + std::string(" logic complement"));

        const bool preauth =
            gameproto::ValidateCommandPolicy(body, gameproto::CommandTrustLevel::PreAuthClient, true,
                                             nullptr) == gameproto::CommandDecision::Allow;
        Expect(preauth == spec->preauth_allow, field->name() + std::string(" preauth"));

        const bool authed =
            gameproto::ValidateCommandPolicy(body, gameproto::CommandTrustLevel::AuthenticatedClient,
                                             false, nullptr) == gameproto::CommandDecision::Allow;
        Expect(authed == spec->authenticated_allow, field->name() + std::string(" authenticated"));
    }
    Expect(seen.empty(), "table has a body_case that is not in the proto");
    Expect(covered == oneof->field_count(), "not every oneof field was checked");
    std::printf("command_route_table_test cases=%d\n", covered);
    if (g_fails) {
        std::printf("%d failures\n", g_fails);
        return 1;
    }
    std::printf("command_route_table_test PASS\n");
    return 0;
}

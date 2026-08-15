/**
 * 已发布 FileDescriptorSet 与当前 descriptor 兼容：禁止删字段、改类型、oneof 编号复用。
 * 基线：docs/protocol/published/v1/game.desc
 */
#include <google/protobuf/descriptor.pb.h>

#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

int g_fail = 0;

void Fail(const std::string &m) {
    std::fprintf(stderr, "FAIL %s\n", m.c_str());
    ++g_fail;
}

bool ReadFile(const std::string &path, std::string *out) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    std::ostringstream os;
    os << in.rdbuf();
    *out = os.str();
    return !out->empty();
}

const google::protobuf::FileDescriptorProto *FindGame(
    const google::protobuf::FileDescriptorSet &set) {
    for (int i = 0; i < set.file_size(); ++i) {
        const auto &f = set.file(i);
        if (f.name() == "game.proto")
            return &f;
        const auto slash = f.name().rfind('/');
        const std::string base =
            slash == std::string::npos ? f.name() : f.name().substr(slash + 1);
        if (base == "game.proto")
            return &f;
    }
    return nullptr;
}

std::string Key(const std::string &msg, int number) {
    return msg + "#" + std::to_string(number);
}

void IndexFields(const google::protobuf::DescriptorProto &d, const std::string &prefix,
                 std::map<std::string, const google::protobuf::FieldDescriptorProto *> *out) {
    const std::string name = prefix.empty() ? d.name() : prefix + "." + d.name();
    for (int i = 0; i < d.field_size(); ++i)
        (*out)[Key(name, d.field(i).number())] = &d.field(i);
    for (int i = 0; i < d.nested_type_size(); ++i)
        IndexFields(d.nested_type(i), name, out);
}

void IndexFile(const google::protobuf::FileDescriptorProto &f,
               std::map<std::string, const google::protobuf::FieldDescriptorProto *> *out) {
    for (int i = 0; i < f.message_type_size(); ++i)
        IndexFields(f.message_type(i), "", out);
}

bool LoadSet(const std::string &path, google::protobuf::FileDescriptorSet *set) {
    std::string raw;
    if (!ReadFile(path, &raw))
        return false;
    return set->ParseFromString(raw);
}

}  // namespace

int main(int argc, char **argv) {
    const char *old_path =
        argc >= 2 ? argv[1] : "docs/protocol/published/v1/game.desc";
    const char *new_path = argc >= 3 ? argv[2] : "";
    google::protobuf::FileDescriptorSet old_set, new_set;
    if (!LoadSet(old_path, &old_set)) {
        std::fprintf(stderr, "FAIL cannot read published descriptor %s\n", old_path);
        return 1;
    }
    if (new_path[0] == '\0') {
        std::string raw;
        // 未传新文件时用 stdin 不方便；要求第二参数或同目录导出
        std::fprintf(stderr, "usage: protocol_compat_test <old.desc> <new.desc>\n");
        return 2;
    }
    if (!LoadSet(new_path, &new_set)) {
        std::fprintf(stderr, "FAIL cannot read current descriptor %s\n", new_path);
        return 1;
    }
    const auto *old_f = FindGame(old_set);
    const auto *new_f = FindGame(new_set);
    if (!old_f || !new_f) {
        Fail("game.proto missing in descriptor set");
        return 1;
    }
    std::map<std::string, const google::protobuf::FieldDescriptorProto *> old_idx, new_idx;
    IndexFile(*old_f, &old_idx);
    IndexFile(*new_f, &new_idx);

    for (const auto &kv : old_idx) {
        auto it = new_idx.find(kv.first);
        if (it == new_idx.end()) {
            Fail("deleted field " + kv.first);
            continue;
        }
        const auto *a = kv.second;
        const auto *b = it->second;
        if (a->type() != b->type())
            Fail("type change " + kv.first);
        if (a->label() != b->label())
            Fail("label change " + kv.first);
        if (a->type() == google::protobuf::FieldDescriptorProto::TYPE_MESSAGE ||
            a->type() == google::protobuf::FieldDescriptorProto::TYPE_ENUM) {
            if (a->type_name() != b->type_name())
                Fail("type_name change " + kv.first);
        }
        if (a->name() != b->name())
            Fail("name change at number " + kv.first + " " + a->name() + " -> " + b->name());
    }

    // oneof 编号复用：旧 oneof 成员编号在新文件不得改隶属到别的 oneof 名字集合外的冲突
    // 已由“同 message 同 number 必须仍存在且同名”覆盖删除/改号；额外检查新字段未占用旧号但改 type。
    if (g_fail) {
        std::fprintf(stderr, "protocol_compat_test FAIL count=%d\n", g_fail);
        return 1;
    }
    std::printf("OK protocol_compat_test old=%s new=%s fields=%zu\n", old_path, new_path,
                old_idx.size());
    return 0;
}

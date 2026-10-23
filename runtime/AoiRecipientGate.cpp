#include "AoiRecipientGate.h"

AoiRecipientGate &AoiRecipientGate::Instance() {
    static AoiRecipientGate g;
    return g;
}

bool AoiRecipientGate::Allowed(uint64_t player_id) const {
    if (player_id == 0)
        return false;
    std::lock_guard<std::mutex> lk(mu_);
    return suppressed_.count(player_id) == 0;
}

void AoiRecipientGate::Suppress(uint64_t player_id) {
    if (player_id == 0)
        return;
    std::lock_guard<std::mutex> lk(mu_);
    suppressed_.insert(player_id);
}

void AoiRecipientGate::Clear(uint64_t player_id) {
    if (player_id == 0)
        return;
    std::lock_guard<std::mutex> lk(mu_);
    suppressed_.erase(player_id);
}

void AoiRecipientGate::ClearAll() {
    std::lock_guard<std::mutex> lk(mu_);
    suppressed_.clear();
}

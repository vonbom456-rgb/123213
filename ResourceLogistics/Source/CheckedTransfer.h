#pragma once
#include <algorithm>
#include <climits>
#include <stdexcept>

namespace StorageSafety {
// Callback-based accounting is independently testable without loading the game.
template<class ReadFrom, class ReadTo, class ChangeFrom, class ChangeTo>
int CheckedTransfer(int wanted, ReadFrom read_from, ReadTo read_to, ChangeFrom change_from, ChangeTo change_to) {
    const int before=read_from(), dest_before=read_to();
    const int amount=std::min({wanted,before,1000});
    if(amount<=0 || dest_before<0 || dest_before>INT_MAX-amount) return 0;
    try {
        change_from(-amount);
        const int removed=before-read_from();
        if(!removed) return 0;
        if(removed<0 || removed>amount) throw std::runtime_error("Unexpected source quantity change");
        change_to(removed);
        const int added=read_to()-dest_before;
        if(added<0 || added>removed) throw std::runtime_error("Unexpected destination quantity change");
        if(added<removed) change_from(removed-added);
        if(read_from()!=before-added || read_to()!=dest_before+added)
            throw std::runtime_error("Transfer quantity mismatch / incomplete rollback");
        return added;
    } catch (...) {
        // A callback may throw after changing a count. Restore only the measured deficit.
        // Never guess the full requested quantity (would duplicate partially added items).
        const int src=read_from(), dst=read_to();
        const long long deficit=static_cast<long long>(before)+dest_before-src-dst;
        if(src>=0 && src<=before && dst>=dest_before && deficit>0 && deficit<=amount)
            change_from(static_cast<int>(deficit));
        throw; // Caller disables further transfers; do not silently resume after a fault.
    }
}
}

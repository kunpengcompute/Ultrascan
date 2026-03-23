#include "pbe_compile.h"

#include "grey.h"
#include "util/target_info.h"

namespace ue2 {

bool canBuildPBE(const target_t &target, const std::vector<hwlmLiteral> &lits,
                 const Grey &grey) {
    if (!grey.allowNeoFdr) {
        return false;
    }

    if (!target.has_neon()) {
        return false;
    }

    if (lits.empty()) {
        return false;
    }

    return true;
}

bool buildPBEArtifacts(const std::vector<hwlmLiteral> &lits,
                       PBECompileArtifacts *artifacts) {
    if (!artifacts) {
        return false;
    }

    artifacts->bitSelectors.clear();
    artifacts->primaryHashTable.offsets.clear();
    artifacts->secondaryHashTable.clear();

    if (lits.empty()) {
        return false;
    }

    // Phase-0 placeholder: reserve an empty slot so layout code can be
    // integrated incrementally without changing APIs.
    artifacts->primaryHashTable.offsets.push_back(0);
    return true;
}

} // namespace ue2

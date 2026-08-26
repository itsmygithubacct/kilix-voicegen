#ifndef KGV_RUNTIME_FRONTEND_PACKAGE_H
#define KGV_RUNTIME_FRONTEND_PACKAGE_H

#include <optional>
#include <string>
#include <vector>

#include "frontend/resolution.h"
#include "runtime/model_package.h"

namespace kgv {

struct VerifiedFrontendPackage final {
    std::vector<SegmentDefinition> segments;
    PronunciationLexicon base_lexicon;
    LtsModel lts;
    ModelTokenInventory model_tokens;
    std::optional<HeteronymRules> heteronym_rules;
    std::optional<MorphologyRules> morphology_rules;
    std::optional<WeakFormRules> weak_form_rules;
    PronunciationAdmission admission = PronunciationAdmission::test_fixture;
    std::string frontend_abi_sha256;

    ResolvedFrontendResources resources() const noexcept;
};

int load_verified_frontend_package(const VerifiedModel &model,
                                   VerifiedFrontendPackage *frontend,
                                   std::string *error);

}  // namespace kgv

#endif

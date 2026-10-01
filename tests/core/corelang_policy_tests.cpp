#include <shared/policy/Policy.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

static constexpr std::string_view POLICY_SOURCE = R"core(@version("0.1.3.1")
@use minecraft
pub fn policy() {
    policyGroup("explicit", 5u8, "")
    policyMember("explicit", 42u64)
    policySelector("mobs", "zombie", 0u64, 99u64, 8u64)
    policyGroup("selected", 6u8, "mobs")
    policyGroup("zombie-kind", 4u8, "zombie")
    policyRule("movement", "minecraft:speed", 200i64, 0u8)
    policyRule("movement", "minecraft:flight", 1i64, 0u8)
    policyAssign("players", "movement", 0u8)
}
)core";

static constexpr std::string_view SECOND_POLICY_SOURCE = R"core(@version("0.1.3.1")
@use minecraft
pub fn policy() {
    policyRule("slow", "minecraft:speed", 1i64, 1u8)
    policyAssign("players", "slow", 1u8)
}
)core";

static constexpr std::string_view NEGATIVE_POLICY_SOURCE = R"core(@version("0.1.3.1")
@use minecraft
pub fn policy() {
    policyRule("slow", "minecraft:speed", -9i64, 0u8)
    policyAssign("players", "slow", 0u8)
}
)core";

static constexpr std::string_view EXPIRED_SUBJECT_POLICY_SOURCE = R"core(@version("0.1.3.1")
@use minecraft
pub fn policy() {
    policyGroup("temporary", 5u8, "")
    policyMember("temporary", 42u64)
    policyRule("grant", "minecraft:flight", 1i64, 0u8)
    policyAssign("temporary", "grant", 0u8)
    policyAssign("entity:42", "grant", 0u8)
}
)core";

static constexpr std::string_view CONTROL_FLOW_POLICY_SOURCE = R"core(@version("0.1.3.1")
@use minecraft
pub fn policy() {
    let mut count = 0u64
    loop {
        count += 1u64
        if count == 1u64 { break }
    }
    policyRule("movement", "minecraft:speed", 200i64, 0u8)
    policyAssign("players", "movement", 0u8)
}
)core";

static constexpr std::string_view RECURSIVE_POLICY_SOURCE = R"core(@version("0.1.3.1")
@use minecraft
fn recurse(value: u64) -> () {
    if value > 0u64 { recurse(value - 1u64) } else { () }
}
pub fn policy() {
    recurse(1u64)
    policyRule("movement", "minecraft:speed", 200i64, 0u8)
    policyAssign("players", "movement", 0u8)
}
)core";

static constexpr std::string_view HELPER_POLICY_SOURCE = R"core(@version("0.1.3.1")
@use minecraft
fn configure() -> () {
    policyRule("movement", "minecraft:speed", 200i64, 0u8)
    policyAssign("players", "movement", 0u8)
}
pub fn policy() {
    configure()
}
)core";

static constexpr std::string_view RESERVED_GROUP_SOURCE = R"core(@version("0.1.3.1")
@use minecraft
pub fn policy() {
    policyGroup("players", 5u8, "")
}
)core";

static constexpr std::string_view PERMISSION_SOURCE = R"core(@version("0.1.3.1")
@use minecraft
pub fn policy() {
    policyGroup("zombie-kind", 4u8, "zombie")
    policyGroup("restricted", 5u8, "")
    policyMember("restricted", 42u64)
    policySelector("zombies", "zombie", 1u64, 9u64, 8u64)
    policyGroup("selected", 6u8, "zombies")
    policyRule("base", "minecraft:flight", 0i64, 0u8)
    policyAssign("all", "base", 0u8)
    policyRule("technical", "minecraft:flight", 1i64, 0u8)
    policyAssign("entities", "technical", 0u8)
    policyRule("players", "minecraft:flight", 1i64, 0u8)
    policyAssign("players", "players", 0u8)
    policyRule("kind", "minecraft:flight", 1i64, 0u8)
    policyAssign("zombie-kind", "kind", 0u8)
    policyRule("selector", "minecraft:flight", 1i64, 0u8)
    policyAssign("selected", "selector", 0u8)
    policyRule("individual", "minecraft:flight", 1i64, 0u8)
    policyAssign("entity:42", "individual", 0u8)
    policyRule("hard-deny", "minecraft:flight", 0i64, 1u8)
    policyAssign("restricted", "hard-deny", 0u8)
}
)core";

static constexpr std::string_view BUILT_IN_ENTITY_KIND_POLICY_SOURCE = R"core(@version("0.1.3.1")
@use minecraft
pub fn policy() {
    policyRule("zombies-grounded", "minecraft:flight", 0i64, 0u8)
    policyAssign("kind:zombie", "zombies-grounded", 0u8)
}
)core";

static constexpr std::string_view BUILT_IN_MOBS_POLICY_SOURCE = R"core(@version("0.1.3.1")
@use minecraft
pub fn policy() {
    policyRule("mobs-fly", "minecraft:flight", 1i64, 0u8)
    policyAssign("mobs", "mobs-fly", 0u8)
}
)core";

static constexpr std::string_view TIED_SOFT_POLICY_SOURCE = R"core(@version("0.1.3.1")
@use minecraft
pub fn policy() {
    policyGroup("deny-members", 5u8, "")
    policyMember("deny-members", 42u64)
    policyGroup("allow-members", 5u8, "")
    policyMember("allow-members", 42u64)
    policyRule("deny", "minecraft:flight", 0i64, 0u8)
    policyAssign("deny-members", "deny", 0u8)
    policyRule("allow", "minecraft:flight", 1i64, 0u8)
    policyAssign("allow-members", "allow", 0u8)
}
)core";

static constexpr std::string_view TIED_SOFT_POLICY_DENY_LAST_SOURCE = R"core(@version("0.1.3.1")
@use minecraft
pub fn policy() {
    policyGroup("deny-members", 5u8, "")
    policyMember("deny-members", 42u64)
    policyGroup("allow-members", 5u8, "")
    policyMember("allow-members", 42u64)
    policyRule("deny", "minecraft:flight", 0i64, 0u8)
    policyRule("allow", "minecraft:flight", 1i64, 0u8)
    policyAssign("allow-members", "allow", 0u8)
    policyAssign("deny-members", "deny", 0u8)
}
)core";

static constexpr std::string_view TIED_HARD_POLICY_SOURCE = R"core(@version("0.1.3.1")
@use minecraft
pub fn policy() {
    policyGroup("deny-members", 5u8, "")
    policyMember("deny-members", 42u64)
    policyGroup("allow-members", 5u8, "")
    policyMember("allow-members", 42u64)
    policyRule("deny", "minecraft:flight", 0i64, 1u8)
    policyAssign("deny-members", "deny", 0u8)
    policyRule("allow", "minecraft:flight", 1i64, 1u8)
    policyAssign("allow-members", "allow", 0u8)
}
)core";

static constexpr std::string_view TIED_HARD_POLICY_ALLOW_FIRST_SOURCE = R"core(@version("0.1.3.1")
@use minecraft
pub fn policy() {
    policyGroup("allow-members", 5u8, "")
    policyMember("allow-members", 42u64)
    policyGroup("deny-members", 5u8, "")
    policyMember("deny-members", 42u64)
    policyRule("allow", "minecraft:flight", 1i64, 1u8)
    policyAssign("allow-members", "allow", 0u8)
    policyRule("deny", "minecraft:flight", 0i64, 1u8)
    policyAssign("deny-members", "deny", 0u8)
}
)core";

static constexpr std::string_view SOFT_INDIVIDUAL_ALLOW_POLICY_SOURCE = R"core(@version("0.1.3.1")
@use minecraft
pub fn policy() {
    policyRule("players-denied", "minecraft:flight", 0i64, 0u8)
    policyAssign("players", "players-denied", 0u8)
    policyRule("individual-allowed", "minecraft:flight", 1i64, 0u8)
    policyAssign("entity:42", "individual-allowed", 0u8)
}
)core";

static constexpr std::string_view SELECTOR_RANGE_POLICY_SOURCE = R"core(@version("0.1.3.1")
@use minecraft
pub fn policy() {
    policySelector("nearby-zombies", "zombie", 9u64, 12u64, 2u64)
    policyGroup("selected-zombies", 6u8, "nearby-zombies")
    policyRule("flight", "minecraft:flight", 1i64, 0u8)
    policyAssign("selected-zombies", "flight", 0u8)
}
)core";

class Cancelled final : public shared::PolicyCancellation {
public:
    [[nodiscard]] bool isCancellationRequested() const noexcept override { return true; }
};

shared::PolicyCompileOptions interpretedOptions()
{
    return {
        .jit_mode = shared::PolicyJitMode::Disabled,
        .require_interpreter_parity = true,
    };
}

shared::PolicyCapabilityRegistry capabilityRegistry()
{
    return {
        .definitions = {
            {
                .key = "minecraft:flight",
                .default_value = 0,
                .minimum_value = 0,
                .maximum_value = 1,
                .hard_restriction = shared::PolicyRestriction::Maximum,
            },
            {
                .key = "minecraft:speed",
                .default_value = 100,
                .minimum_value = 0,
                .maximum_value = 200,
                .hard_restriction = shared::PolicyRestriction::Maximum,
            },
        },
    };
}

TEST(CoreLangPolicyTest, CompilesTypedDeclarationsDeterministicallyWithInterpreterParity)
{
    shared::PolicyHost const host;
    auto const first = host.compile("policy.core", POLICY_SOURCE, interpretedOptions());
    auto const second = host.compile("policy.core", POLICY_SOURCE, interpretedOptions());

    ASSERT_TRUE(first.has_value()) << first.error().message;
    ASSERT_TRUE(second.has_value()) << second.error().message;
    EXPECT_EQ(first->plan, second->plan);
    EXPECT_EQ(shared::policyPlanId(first->plan), shared::policyPlanId(second->plan));
    EXPECT_EQ(first->plan.statementCount(), 8U);
    ASSERT_EQ(first->plan.groups().size(), 3U);
    EXPECT_EQ(first->plan.groups()[0].members, std::vector<shared::PolicyEntityId>{42U});
    ASSERT_EQ(first->plan.selectors().size(), 1U);
    EXPECT_EQ(first->plan.selectors()[0].max_results, 8U);
    EXPECT_EQ(first->plan.groups()[2].entity_kind, "zombie");
    ASSERT_EQ(first->plan.presets().size(), 1U);
    ASSERT_EQ(first->plan.presets()[0].rules.size(), 2U);
    EXPECT_FALSE(first->plan.presets()[0].rules[1].hard);
    ASSERT_EQ(first->plan.assignments().size(), 1U);
    EXPECT_TRUE(first->interpreter_parity);
    EXPECT_FALSE(first->jit_prepared);
    EXPECT_EQ(first->jit_mode, shared::PolicyJitMode::Disabled);
}

TEST(CoreLangPolicyTest, PreservesInterpreterParityWhenJitIsPreferred)
{
    shared::PolicyHost const host;
    auto const interpreted = host.compile("policy.core", POLICY_SOURCE, interpretedOptions());
    auto const preferred = host.compile("policy.core", POLICY_SOURCE, {
        .jit_mode = shared::PolicyJitMode::Preferred,
        .require_interpreter_parity = true,
    });

    ASSERT_TRUE(interpreted.has_value()) << interpreted.error().message;
    ASSERT_TRUE(preferred.has_value()) << preferred.error().message;
    EXPECT_EQ(preferred->plan, interpreted->plan);
    EXPECT_EQ(preferred->jit_mode, shared::PolicyJitMode::Preferred);
    EXPECT_TRUE(preferred->interpreter_parity);
}

TEST(CoreLangPolicyTest, RequiredJitNeverSilentlyFallsBackToTheInterpreter)
{
    shared::PolicyHost const host;
    auto const interpreted = host.compile("policy.core", POLICY_SOURCE, interpretedOptions());
    ASSERT_TRUE(interpreted.has_value()) << interpreted.error().message;

    auto const required = host.compile("policy.core", POLICY_SOURCE, {
        .jit_mode = shared::PolicyJitMode::Required,
        .require_interpreter_parity = true,
    });
#if (defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))) \
    || (defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64))) \
    || (defined(__ANDROID__) && (defined(__aarch64__) || defined(_M_ARM64)))
    ASSERT_TRUE(required.has_value()) << required.error().message;
#endif
    if (!required) {
        EXPECT_EQ(required.error().code, shared::PolicyDiagnosticCode::JitPreparationFailure);
        EXPECT_EQ(host.snapshot(), nullptr);
        return;
    }

    EXPECT_EQ(required->plan, interpreted->plan);
    EXPECT_EQ(required->jit_mode, shared::PolicyJitMode::Required);
    EXPECT_TRUE(required->jit_prepared);
    EXPECT_TRUE(required->interpreter_parity);
}

TEST(CoreLangPolicyTest, DecodesNegativePermissionValuesWithoutOverflow)
{
    shared::PolicyHost host;
    auto compiled = host.compile("negative.core", NEGATIVE_POLICY_SOURCE, interpretedOptions());
    ASSERT_TRUE(compiled.has_value()) << compiled.error().message;

    shared::PolicyCapabilityRegistry const registry{
        .definitions = {{
            .key = "minecraft:speed",
            .default_value = 0,
            .minimum_value = -10,
            .maximum_value = 10,
            .hard_restriction = shared::PolicyRestriction::Maximum,
        }},
    };
    std::vector<shared::PolicySubject> const subjects{
        {.id = 42U, .entity_class = shared::PolicyEntityClass::Player, .kind = "player"},
    };
    auto const materialized = host.materialize(compiled->plan, subjects, registry);

    ASSERT_TRUE(materialized.has_value()) << materialized.error().message;
    ASSERT_EQ(compiled->plan.presets().size(), 1U);
    ASSERT_EQ(compiled->plan.presets()[0].rules.size(), 1U);
    EXPECT_EQ(compiled->plan.presets()[0].rules[0].value, -9);
    EXPECT_EQ(materialized->value(42U, 0U), -9);
}

TEST(CoreLangPolicyTest, MaterializesGeneralEntityCapabilitiesBeforeAtomicPublication)
{
    shared::PolicyHost host;
    auto compiled = host.compile("permissions.core", PERMISSION_SOURCE, interpretedOptions());
    ASSERT_TRUE(compiled.has_value()) << compiled.error().message;

    std::vector<shared::PolicySubject> const subjects{
        {.id = 42U, .entity_class = shared::PolicyEntityClass::Player, .kind = "player"},
        {.id = 7U, .entity_class = shared::PolicyEntityClass::Player, .kind = "player"},
        {.id = 9U, .entity_class = shared::PolicyEntityClass::Mob, .kind = "zombie"},
        {.id = 10U, .entity_class = shared::PolicyEntityClass::TechnicalEntity, .kind = "missile"},
    };
    auto materialized = host.materialize(compiled->plan, subjects, capabilityRegistry());
    ASSERT_TRUE(materialized.has_value()) << materialized.error().message;
    EXPECT_EQ(materialized->capabilityCount(), 2U);
    EXPECT_FALSE(materialized->allows(42U, 0U));
    EXPECT_TRUE(materialized->allows(7U, 0U));
    EXPECT_TRUE(materialized->allows(9U, 0U));
    EXPECT_TRUE(materialized->allows(10U, 0U));
    EXPECT_FALSE(materialized->allows(999U, 0U));
    EXPECT_FALSE(materialized->allows(7U, 9U));
    EXPECT_EQ(materialized->value(7U, 1U), 100);

    shared::PolicyLimits limited_limits = shared::defaultPolicyLimits();
    limited_limits.max_subjects = 1U;
    shared::PolicyHost const limited_host{limited_limits};
    auto const limited = limited_host.materialize(compiled->plan, subjects, capabilityRegistry());
    ASSERT_FALSE(limited.has_value());
    EXPECT_EQ(limited.error().code, shared::PolicyDiagnosticCode::InvalidDeclaration);

    auto const generation = host.publish(std::move(*compiled), std::move(*materialized));
    ASSERT_TRUE(generation.has_value()) << generation.error().message;
    auto const snapshot = host.snapshot();
    ASSERT_NE(snapshot, nullptr);
    auto const capabilities = snapshot->capabilities();
    ASSERT_NE(capabilities, nullptr);
    EXPECT_EQ(capabilities->generation(), *generation);
    EXPECT_TRUE(capabilities->allows(9U, 0U));
}

TEST(CoreLangPolicyTest, DenialWinsTiedSoftAssignmentsRegardlessOfDeclarationOrder)
{
    shared::PolicyHost const host;
    std::vector<shared::PolicySubject> const subjects{
        {.id = 42U, .entity_class = shared::PolicyEntityClass::Player, .kind = "player"},
    };
    for (std::string_view const source : {TIED_SOFT_POLICY_SOURCE, TIED_SOFT_POLICY_DENY_LAST_SOURCE}) {
        auto const compiled = host.compile("tied-soft.core", source, interpretedOptions());
        ASSERT_TRUE(compiled.has_value()) << compiled.error().message;
        auto const materialized = host.materialize(compiled->plan, subjects, capabilityRegistry());

        ASSERT_TRUE(materialized.has_value()) << materialized.error().message;
        EXPECT_FALSE(materialized->allows(42U, 0U));
    }
}

TEST(CoreLangPolicyTest, RejectsUnknownEntityClassesDuringMaterialization)
{
    shared::PolicyHost const host;
    auto const compiled = host.compile("invalid-subject-class.core", POLICY_SOURCE, interpretedOptions());
    ASSERT_TRUE(compiled.has_value()) << compiled.error().message;

    std::vector<shared::PolicySubject> const subjects{
        {
            .id = 42U,
            .entity_class = static_cast<shared::PolicyEntityClass>(255U),
            .kind = "player",
        },
    };
    auto const materialized = host.materialize(compiled->plan, subjects, capabilityRegistry());

    ASSERT_FALSE(materialized.has_value());
    EXPECT_EQ(materialized.error().code, shared::PolicyDiagnosticCode::InvalidDeclaration);
    EXPECT_EQ(materialized.error().message, "policy subject catalog is invalid");
}

TEST(CoreLangPolicyTest, HardRestrictionsWinTiesRegardlessOfDeclarationOrder)
{
    shared::PolicyHost const host;
    std::vector<shared::PolicySubject> const subjects{
        {.id = 42U, .entity_class = shared::PolicyEntityClass::Player, .kind = "player"},
    };
    for (std::string_view const source : {TIED_HARD_POLICY_SOURCE, TIED_HARD_POLICY_ALLOW_FIRST_SOURCE}) {
        auto const compiled = host.compile("tied-hard.core", source, interpretedOptions());
        ASSERT_TRUE(compiled.has_value()) << compiled.error().message;
        auto const materialized = host.materialize(compiled->plan, subjects, capabilityRegistry());

        ASSERT_TRUE(materialized.has_value()) << materialized.error().message;
        EXPECT_FALSE(materialized->allows(42U, 0U));
    }
}

TEST(CoreLangPolicyTest, IndividualSoftAllowOverridesGroupSoftDenyOnlyForThatEntity)
{
    shared::PolicyHost const host;
    auto const compiled = host.compile(
        "individual-soft-allow.core", SOFT_INDIVIDUAL_ALLOW_POLICY_SOURCE, interpretedOptions()
    );
    ASSERT_TRUE(compiled.has_value()) << compiled.error().message;

    std::vector<shared::PolicySubject> const subjects{
        {.id = 42U, .entity_class = shared::PolicyEntityClass::Player, .kind = "player"},
        {.id = 7U, .entity_class = shared::PolicyEntityClass::Player, .kind = "player"},
    };
    auto const materialized = host.materialize(compiled->plan, subjects, capabilityRegistry());

    ASSERT_TRUE(materialized.has_value()) << materialized.error().message;
    EXPECT_TRUE(materialized->allows(42U, 0U));
    EXPECT_FALSE(materialized->allows(7U, 0U));
}

TEST(CoreLangPolicyTest, ExpiringSubjectRemovesConcreteMembershipAndOverrides)
{
    shared::PolicyHost const host;
    auto const compiled = host.compile(
        "expiring-subject.core", EXPIRED_SUBJECT_POLICY_SOURCE, interpretedOptions()
    );
    ASSERT_TRUE(compiled.has_value()) << compiled.error().message;
    std::vector<shared::PolicySubject> const subjects{
        {.id = 42U, .entity_class = shared::PolicyEntityClass::Player, .kind = "player"},
    };
    auto const before_expiration = host.materialize(compiled->plan, subjects, capabilityRegistry());
    ASSERT_TRUE(before_expiration.has_value()) << before_expiration.error().message;
    ASSERT_TRUE(before_expiration->allows(42U, 0U));

    shared::PolicyPlan const expired = compiled->plan.withoutSubject(42U);
    ASSERT_EQ(compiled->plan.groups()[0].members, std::vector<shared::PolicyEntityId>{42U});
    EXPECT_TRUE(expired.groups()[0].members.empty());
    ASSERT_EQ(expired.assignments().size(), 1U);
    EXPECT_EQ(expired.assignments()[0].target, "temporary");
    EXPECT_NE(shared::policyPlanId(expired), shared::policyPlanId(compiled->plan));
    EXPECT_TRUE(before_expiration->allows(42U, 0U));

    auto const reused_subject = host.materialize(expired, subjects, capabilityRegistry());
    ASSERT_TRUE(reused_subject.has_value()) << reused_subject.error().message;
    EXPECT_FALSE(reused_subject->allows(42U, 0U));
}

TEST(CoreLangPolicyTest, SelectorRangeMatchesInclusiveIdsOnlyWithinItsEntityKind)
{
    shared::PolicyHost const host;
    auto const compiled = host.compile(
        "selector-range.core", SELECTOR_RANGE_POLICY_SOURCE, interpretedOptions()
    );
    ASSERT_TRUE(compiled.has_value()) << compiled.error().message;

    std::vector<shared::PolicySubject> subjects{
        {.id = 8U, .entity_class = shared::PolicyEntityClass::Mob, .kind = "zombie"},
        {.id = 9U, .entity_class = shared::PolicyEntityClass::Mob, .kind = "zombie"},
        {.id = 12U, .entity_class = shared::PolicyEntityClass::Mob, .kind = "zombie"},
        {.id = 13U, .entity_class = shared::PolicyEntityClass::Mob, .kind = "zombie"},
        {.id = 11U, .entity_class = shared::PolicyEntityClass::TechnicalEntity, .kind = "missile"},
    };
    auto const materialized = host.materialize(compiled->plan, subjects, capabilityRegistry());

    ASSERT_TRUE(materialized.has_value()) << materialized.error().message;
    EXPECT_TRUE(materialized->allows(9U, 0U));
    EXPECT_TRUE(materialized->allows(12U, 0U));
    EXPECT_FALSE(materialized->allows(8U, 0U));
    EXPECT_FALSE(materialized->allows(11U, 0U));
    EXPECT_FALSE(materialized->allows(13U, 0U));

    subjects.clear();
    EXPECT_TRUE(materialized->allows(9U, 0U));
    EXPECT_TRUE(materialized->allows(12U, 0U));
}

TEST(CoreLangPolicyTest, SelectorRejectsReversedIdRanges)
{
    shared::PolicyHost const host;
    std::string const source{R"core(@version("0.1.3.1")
@use minecraft
pub fn policy() {
    policySelector("invalid", "zombie", 10u64, 9u64, 2u64)
}
)core"};

    auto const compiled = host.compile("invalid-selector-range.core", source, interpretedOptions());

    ASSERT_FALSE(compiled.has_value());
    EXPECT_EQ(compiled.error().code, shared::PolicyDiagnosticCode::InvalidDeclaration);
}

TEST(CoreLangPolicyTest, BuiltInEntityKindGroupTargetsEverySubjectOfOnlyThatKind)
{
    shared::PolicyHost const host;
    auto const compiled = host.compile(
        "kind-policy.core", BUILT_IN_ENTITY_KIND_POLICY_SOURCE, interpretedOptions()
    );
    ASSERT_TRUE(compiled.has_value()) << compiled.error().message;

    std::vector<shared::PolicySubject> const subjects{
        {.id = 42U, .entity_class = shared::PolicyEntityClass::Player, .kind = "player"},
        {.id = 9U, .entity_class = shared::PolicyEntityClass::Mob, .kind = "zombie"},
        {.id = 10U, .entity_class = shared::PolicyEntityClass::TechnicalEntity, .kind = "missile"},
    };
    auto registry = capabilityRegistry();
    registry.definitions[0].default_value = 1;
    auto const materialized = host.materialize(compiled->plan, subjects, registry);

    ASSERT_TRUE(materialized.has_value()) << materialized.error().message;
    EXPECT_TRUE(materialized->allows(42U, 0U));
    EXPECT_FALSE(materialized->allows(9U, 0U));
    EXPECT_TRUE(materialized->allows(10U, 0U));
}

TEST(CoreLangPolicyTest, BuiltInMobsGroupTargetsOnlyMobSubjects)
{
    shared::PolicyHost const host;
    auto const compiled = host.compile(
        "mobs-policy.core", BUILT_IN_MOBS_POLICY_SOURCE, interpretedOptions()
    );
    ASSERT_TRUE(compiled.has_value()) << compiled.error().message;

    std::vector<shared::PolicySubject> const subjects{
        {.id = 42U, .entity_class = shared::PolicyEntityClass::Player, .kind = "player"},
        {.id = 9U, .entity_class = shared::PolicyEntityClass::Mob, .kind = "zombie"},
        {.id = 10U, .entity_class = shared::PolicyEntityClass::TechnicalEntity, .kind = "missile"},
    };
    auto const materialized = host.materialize(compiled->plan, subjects, capabilityRegistry());

    ASSERT_TRUE(materialized.has_value()) << materialized.error().message;
    EXPECT_FALSE(materialized->allows(42U, 0U));
    EXPECT_TRUE(materialized->allows(9U, 0U));
    EXPECT_FALSE(materialized->allows(10U, 0U));
}

TEST(CoreLangPolicyTest, CustomGroupCannotUseIndividualEntityTargetNamespace)
{
    shared::PolicyHost const host;
    std::string const source{R"core(@version("0.1.3.1")
@use minecraft
pub fn policy() {
    policyGroup("entity:42", 5u8, "")
}
)core"};

    auto const compiled = host.compile("reserved-entity-group.core", source, interpretedOptions());

    ASSERT_FALSE(compiled.has_value());
    EXPECT_EQ(compiled.error().code, shared::PolicyDiagnosticCode::InvalidDeclaration);
}

TEST(CoreLangPolicyTest, WorldSubjectNamespaceIsReservedWithoutDefiningWorldPermissions)
{
    shared::PolicyHost const host;
    for (std::string_view const group_name : {"world", "world:overworld"}) {
        std::string const source{
            "@version(\"0.1.3.1\")\n@use minecraft\npub fn policy() {\n"
            "    policyGroup(\"" + std::string{group_name} + "\", 5u8, \"\")\n}\n"
        };
        auto const compiled = host.compile("reserved-world-group.core", source, interpretedOptions());

        ASSERT_FALSE(compiled.has_value()) << group_name;
        EXPECT_EQ(compiled.error().code, shared::PolicyDiagnosticCode::InvalidDeclaration) << group_name;
    }
    for (std::string_view const target : {"world", "world:overworld"}) {
        std::string const source{
            "@version(\"0.1.3.1\")\n@use minecraft\npub fn policy() {\n"
            "    policyRule(\"base\", \"minecraft:flight\", 0i64, 0u8)\n"
            "    policyAssign(\"" + std::string{target} + "\", \"base\", 0u8)\n}\n"
        };
        auto const compiled = host.compile("reserved-world-target.core", source, interpretedOptions());

        ASSERT_FALSE(compiled.has_value()) << target;
        EXPECT_EQ(compiled.error().code, shared::PolicyDiagnosticCode::InvalidDeclaration) << target;
    }
}

TEST(CoreLangPolicyTest, CustomGroupCannotRedefineBuiltInEntityKindTarget)
{
    shared::PolicyHost const host;
    std::string const source{R"core(@version("0.1.3.1")
@use minecraft
pub fn policy() {
    policyGroup("kind:zombie", 4u8, "zombie")
}
)core"};

    auto const compiled = host.compile("reserved-kind-group.core", source, interpretedOptions());

    ASSERT_FALSE(compiled.has_value());
    EXPECT_EQ(compiled.error().code, shared::PolicyDiagnosticCode::InvalidDeclaration);
}

TEST(CoreLangPolicyTest, CustomGroupsCannotAliasFixedBroadGroups)
{
    shared::PolicyHost const host;
    for (uint8_t kind = 0U; kind < 4U; ++kind) {
        std::string source{
            "@version(\"0.1.3.1\")\n@use minecraft\npub fn policy() {\n"
            "    policyGroup(\"custom-broad\", "
        };
        source += std::to_string(kind);
        source += "u8, \"\")\n}\n";

        auto const compiled = host.compile("custom-broad.core", source, interpretedOptions());

        EXPECT_FALSE(compiled.has_value()) << "group kind " << static_cast<unsigned>(kind);
        if (!compiled) {
            EXPECT_EQ(compiled.error().code, shared::PolicyDiagnosticCode::InvalidDeclaration)
                << "group kind " << static_cast<unsigned>(kind);
        }
    }
}

TEST(CoreLangPolicyTest, RejectsUnknownCapabilitiesAndSelectorOverflowsBeforePublication)
{
    shared::PolicyHost host;
    auto compiled = host.compile("permissions.core", PERMISSION_SOURCE, interpretedOptions());
    ASSERT_TRUE(compiled.has_value()) << compiled.error().message;
    auto registry = capabilityRegistry();
    registry.definitions[0].key = "minecraft:other";
    auto const unregistered = host.materialize(compiled->plan, {}, registry);
    ASSERT_FALSE(unregistered.has_value());
    EXPECT_EQ(unregistered.error().code, shared::PolicyDiagnosticCode::UnknownDeclaration);

    std::vector<shared::PolicySubject> const zombies{
        {.id = 42U, .entity_class = shared::PolicyEntityClass::Player, .kind = "player"},
        {.id = 1U, .entity_class = shared::PolicyEntityClass::Mob, .kind = "zombie"},
        {.id = 2U, .entity_class = shared::PolicyEntityClass::Mob, .kind = "zombie"},
        {.id = 3U, .entity_class = shared::PolicyEntityClass::Mob, .kind = "zombie"},
        {.id = 4U, .entity_class = shared::PolicyEntityClass::Mob, .kind = "zombie"},
        {.id = 5U, .entity_class = shared::PolicyEntityClass::Mob, .kind = "zombie"},
        {.id = 6U, .entity_class = shared::PolicyEntityClass::Mob, .kind = "zombie"},
        {.id = 7U, .entity_class = shared::PolicyEntityClass::Mob, .kind = "zombie"},
        {.id = 8U, .entity_class = shared::PolicyEntityClass::Mob, .kind = "zombie"},
        {.id = 9U, .entity_class = shared::PolicyEntityClass::Mob, .kind = "zombie"},
    };
    auto const overflow = host.materialize(compiled->plan, zombies, capabilityRegistry());
    ASSERT_FALSE(overflow.has_value());
    EXPECT_EQ(overflow.error().code, shared::PolicyDiagnosticCode::SelectorLimitExceeded);
    EXPECT_EQ(host.snapshot(), nullptr);

    auto const reserved_group = host.compile("reserved-group.core", RESERVED_GROUP_SOURCE, interpretedOptions());
    ASSERT_FALSE(reserved_group.has_value());
    EXPECT_EQ(reserved_group.error().code, shared::PolicyDiagnosticCode::InvalidDeclaration);
}

TEST(CoreLangPolicyTest, EnforcesHostLimitsAndCancellationBeforePublication)
{
    shared::PolicyLimits limits = shared::defaultPolicyLimits();
    limits.max_statements = 2U;
    shared::PolicyHost const limited_host{limits};
    auto const limited = limited_host.compile("limited.core", POLICY_SOURCE, interpretedOptions());
    ASSERT_FALSE(limited.has_value());
    EXPECT_EQ(limited.error().code, shared::PolicyDiagnosticCode::StatementLimitExceeded);
    EXPECT_EQ(limited_host.snapshot(), nullptr);

    Cancelled const cancellation;
    shared::PolicyHost const cancellable_host;
    auto const cancelled = cancellable_host.compile(
        "cancelled.core", POLICY_SOURCE, interpretedOptions(), &cancellation
    );
    ASSERT_FALSE(cancelled.has_value());
    EXPECT_EQ(cancelled.error().code, shared::PolicyDiagnosticCode::Cancelled);
    EXPECT_EQ(cancellable_host.snapshot(), nullptr);
}

TEST(CoreLangPolicyTest, EnforcesStaticInstructionBudgetBeforeExecution)
{
    shared::PolicyLimits limits = shared::defaultPolicyLimits();
    limits.max_instructions = 1U;
    shared::PolicyHost const host{limits};

    auto const compilation = host.compile("instruction-budget.core", POLICY_SOURCE, interpretedOptions());

    ASSERT_FALSE(compilation.has_value());
    EXPECT_EQ(
        shared::policyDiagnosticCodeName(compilation.error().code),
        "instruction-limit-exceeded"
    );
    EXPECT_EQ(host.snapshot(), nullptr);
}

TEST(CoreLangPolicyTest, RejectsPolicyControlFlowCycles)
{
    shared::PolicyHost const host;
    auto const compilation = host.compile(
        "control-flow.core", CONTROL_FLOW_POLICY_SOURCE, interpretedOptions()
    );

    ASSERT_FALSE(compilation.has_value());
    EXPECT_EQ(
        shared::policyDiagnosticCodeName(compilation.error().code),
        "unbounded-execution"
    );
    EXPECT_EQ(host.snapshot(), nullptr);
}

TEST(CoreLangPolicyTest, RejectsRecursiveFunctionCalls)
{
    shared::PolicyHost const host;
    auto const compilation = host.compile(
        "recursive.core", RECURSIVE_POLICY_SOURCE, interpretedOptions()
    );

    ASSERT_FALSE(compilation.has_value());
    EXPECT_EQ(compilation.error().code, shared::PolicyDiagnosticCode::UnboundedExecution)
        << compilation.error().message;
    EXPECT_EQ(host.snapshot(), nullptr);
}

TEST(CoreLangPolicyTest, AllowsAcyclicHelperCallsWithinTheExecutionBudget)
{
    shared::PolicyHost const host;
    auto const compilation = host.compile(
        "helper.core", HELPER_POLICY_SOURCE, interpretedOptions()
    );

    ASSERT_TRUE(compilation.has_value()) << compilation.error().message;
    EXPECT_EQ(compilation->plan.statementCount(), 2U);
}

TEST(CoreLangPolicyTest, RejectsCapabilitiesMaterializedForAnotherPlan)
{
    shared::PolicyHost host;
    auto first = host.compile("first.core", POLICY_SOURCE, interpretedOptions());
    auto second = host.compile("second.core", SECOND_POLICY_SOURCE, interpretedOptions());
    ASSERT_TRUE(first.has_value()) << first.error().message;
    ASSERT_TRUE(second.has_value()) << second.error().message;

    std::vector<shared::PolicySubject> const subjects{
        {.id = 42U, .entity_class = shared::PolicyEntityClass::Player, .kind = "player"},
    };
    shared::PolicyCapabilityRegistry const registry = capabilityRegistry();
    auto first_capabilities = host.materialize(first->plan, subjects, registry);
    auto second_capabilities = host.materialize(second->plan, subjects, registry);
    ASSERT_TRUE(first_capabilities.has_value()) << first_capabilities.error().message;
    ASSERT_TRUE(second_capabilities.has_value()) << second_capabilities.error().message;

    auto const publication = host.publish(std::move(*first), std::move(*second_capabilities));

    ASSERT_FALSE(publication.has_value());
    EXPECT_EQ(
        shared::policyDiagnosticCodeName(publication.error().code),
        "materialization-mismatch"
    );
    EXPECT_EQ(host.snapshot(), nullptr);
}

TEST(CoreLangPolicyTest, RejectsPublicationWithoutMaterializedCapabilities)
{
    shared::PolicyHost host;
    auto compilation = host.compile("missing-capabilities.core", POLICY_SOURCE, interpretedOptions());
    ASSERT_TRUE(compilation.has_value()) << compilation.error().message;

    auto const publication = host.publish(std::move(*compilation));

    ASSERT_FALSE(publication.has_value());
    EXPECT_EQ(
        shared::policyDiagnosticCodeName(publication.error().code),
        "missing-materialization"
    );
    EXPECT_EQ(host.snapshot(), nullptr);
}

TEST(CoreLangPolicyTest, PublishesCompleteCandidatesAtomicallyWithGenerations)
{
    shared::PolicyHost host;
    auto first = host.compile("first.core", POLICY_SOURCE, interpretedOptions());
    auto second = host.compile("second.core", SECOND_POLICY_SOURCE, interpretedOptions());
    ASSERT_TRUE(first.has_value()) << first.error().message;
    ASSERT_TRUE(second.has_value()) << second.error().message;

    std::vector<shared::PolicySubject> const subjects{
        {.id = 42U, .entity_class = shared::PolicyEntityClass::Player, .kind = "player"},
    };
    shared::PolicyCapabilityRegistry const registry = capabilityRegistry();
    auto first_capabilities = host.materialize(first->plan, subjects, registry);
    auto second_capabilities = host.materialize(second->plan, subjects, registry);
    ASSERT_TRUE(first_capabilities.has_value()) << first_capabilities.error().message;
    ASSERT_TRUE(second_capabilities.has_value()) << second_capabilities.error().message;

    auto const first_generation = host.publish(std::move(*first), std::move(*first_capabilities));
    ASSERT_TRUE(first_generation.has_value()) << first_generation.error().message;
    auto const first_snapshot = host.snapshot();
    ASSERT_NE(first_snapshot, nullptr);
    EXPECT_NE(first_snapshot->capabilities(), nullptr);
    EXPECT_EQ(*first_generation, 1U);
    EXPECT_EQ(first_snapshot->generation(), 1U);
    EXPECT_EQ(first_snapshot->plan().sourceId(), "first.core");

    auto const second_generation = host.publish(std::move(*second), std::move(*second_capabilities));
    ASSERT_TRUE(second_generation.has_value()) << second_generation.error().message;
    auto const second_snapshot = host.snapshot();
    ASSERT_NE(second_snapshot, nullptr);
    EXPECT_NE(second_snapshot->capabilities(), nullptr);
    EXPECT_EQ(*second_generation, 2U);
    EXPECT_EQ(second_snapshot->generation(), 2U);
    EXPECT_EQ(second_snapshot->plan().sourceId(), "second.core");
    EXPECT_EQ(first_snapshot->generation(), 1U);
    EXPECT_EQ(first_snapshot->plan().sourceId(), "first.core");
}

} // namespace

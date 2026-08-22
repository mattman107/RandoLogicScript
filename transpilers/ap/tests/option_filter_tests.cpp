// Tests for OptionFilter generation: how `setting()` expressions become RuleBuilder
// OptionFilters wrapped in standalone rules. This is generic AP behavior owned by the
// base ApTranspiler, so it is tested against a minimal default-hook transpiler with no
// game-specific rendering. Game-specific renderings (enum prefixes, host-call rewrites)
// are covered by each derived transpiler's own tests.
#include "helpers.h"

using namespace rls::transpilers::ap_tests;

namespace {
struct ResolvedExpression {
	rls::ast::Project project;
	rls::ast::ExprPtr expr;
};
} // namespace

static std::string GenerateExpression(const ResolvedExpression& resolved) {
	return TestApTranspiler(resolved.project).GenerateExpression(resolved.expr);
}

// Resolve a define from inline RLS source and hand back its body expression.
static ResolvedExpression sourceToExpression(const std::string& source, const std::string& defineName) {
	auto project = resolveFromSource(source);
	auto defineDecl = project.DefineDecls.find(defineName);
	if (defineDecl == project.DefineDecls.end()) {
		return { std::move(project), nullptr };
	}

	return {
		std::move(project),
		std::move(const_cast<rls::ast::DefineDecl*>(defineDecl->second)->body)
	};
}

namespace {
struct GenResult {
	std::string output;
	std::vector<rls::ast::Diagnostic> diagnostics;
};
} // namespace

// As GenerateExpression, but also hands back the diagnostics raised while generating.
static GenResult generateWithDiagnostics(const std::string& source, const std::string& defineName) {
	auto project = resolveFromSource(source);
	auto it = project.DefineDecls.find(defineName);
	if (it == project.DefineDecls.end()) {
		ADD_FAILURE() << "define not found: " << defineName;
		return {};
	}
	TestApTranspiler transpiler(project);
	std::string output = transpiler.GenerateExpression(it->second->body);
	return { std::move(output), transpiler.Diagnostics() };
}

// A bare OptionFilter is not a RuleBuilder Rule, so a `setting(KEY) is VALUE` comparison
// is wrapped in its own True_(options=[...]) rule.
TEST(ApOptionFilters, SettingComparisonIsWrappedAsRule) {
	EXPECT_EQ(GenerateExpression(sourceToExpression(
		"define test():\n"
		"    setting(RSK_FOO) is RO_BAR\n",
		"test")),
		"True_(options=[OptionFilter(RSK_FOO, RO_BAR)])");
}

// `is not` carries the "ne" operator through into the OptionFilter.
TEST(ApOptionFilters, NotEqualSettingComparisonUsesNeOperator) {
	EXPECT_EQ(GenerateExpression(sourceToExpression(
		"define test():\n"
		"    setting(RSK_FOO) is not RO_BAR\n",
		"test")),
		"True_(options=[OptionFilter(RSK_FOO, RO_BAR, \"ne\")])");
}

// A bare setting() call is a truthiness check, emitted as OptionFilter(KEY, True).
TEST(ApOptionFilters, BareSettingCallChecksTrue) {
	EXPECT_EQ(GenerateExpression(sourceToExpression(
		"define test():\n"
		"    setting(RSK_FOO)\n",
		"test")),
		"True_(options=[OptionFilter(RSK_FOO, True)])");
}

// `not setting()` is the negated truthiness check, OptionFilter(KEY, False).
TEST(ApOptionFilters, NegatedSettingCallChecksFalse) {
	EXPECT_EQ(GenerateExpression(sourceToExpression(
		"define test():\n"
		"    not setting(RSK_FOO)\n",
		"test")),
		"True_(options=[OptionFilter(RSK_FOO, False)])");
}

// Two setting comparisons OR'd together cannot combine as bare OptionFilters in RuleBuilder.
// Each must be wrapped in its own rule and joined with `|` so it stays a valid Or of rules.
TEST(ApOptionFilters, AdjacentOrOfSettingsBecomesSeparateWrappedRules) {
	EXPECT_EQ(GenerateExpression(sourceToExpression(
		"define test():\n"
		"    setting(RSK_FOO) is RO_BAR or setting(RSK_FOO) is RO_BAZ\n",
		"test")),
		"True_(options=[OptionFilter(RSK_FOO, RO_BAR)]) | "
		"True_(options=[OptionFilter(RSK_FOO, RO_BAZ)])");
}

// The same applies to AND: each wrapped rule joins with `&`.
TEST(ApOptionFilters, AdjacentAndOfSettingsBecomesSeparateWrappedRules) {
	EXPECT_EQ(GenerateExpression(sourceToExpression(
		"define test():\n"
		"    setting(RSK_FOO) is RO_BAR and setting(RSK_QUX) is RO_BAZ\n",
		"test")),
		"True_(options=[OptionFilter(RSK_FOO, RO_BAR)]) & "
		"True_(options=[OptionFilter(RSK_QUX, RO_BAZ)])");
}

// A setting comparison combined with a real rule needs no extra parentheses around the
// wrapped OptionFilter rule: it is emitted as an atomic call, not a Python comparison.
TEST(ApOptionFilters, SettingComparisonMixedWithRuleHasNoExtraParens) {
	EXPECT_EQ(GenerateExpression(sourceToExpression(
		"define test():\n"
		"    has(RG_HOOKSHOT) or setting(RSK_FOO) is RO_BAR\n",
		"test")),
		"has(RG_HOOKSHOT) | "
		"True_(options=[OptionFilter(RSK_FOO, RO_BAR)])");
}

// Complex rule to test parenthesis
TEST(ApOptionFilters, ComplexSettingParens) {
	EXPECT_EQ(GenerateExpression(sourceToExpression(
		"define test():\n"
		"    has(RG_HOOKSHOT) or setting(RSK_FOO) is RO_BAR and (setting(RSK_BAR) is RO_FOO or can_kill(RE_GOLD_SKULLTULA)) and (flag(LOGIC_BAZ) or setting(RSK_BAZ) is RO_GENERIC_YES)\n",
		"test")),
		"has(RG_HOOKSHOT) | "
		"True_(options=[OptionFilter(RSK_FOO, RO_BAR)]) & "
		"(True_(options=[OptionFilter(RSK_BAR, RO_FOO)]) | can_kill(RE_GOLD_SKULLTULA)) & "
		"(flag(LOGIC_BAZ) | True_(options=[OptionFilter(RSK_BAZ, RO_GENERIC_YES)]))");
}

// == Shapes the comparison can take ==========================================
// A setting comparison must be recognized however the source spells it: an OptionFilter is
// the only lowering, and anything that falls through emits a raw Python operation against a
// Rule object (silently False for ==, a TypeError for an ordered comparison).

// The dotted `Enum.Value` disambiguation form is the same comparison as the bare value.
TEST(ApOptionFilters, DottedEnumValueIsRecognized) {
	GenResult result = generateWithDiagnostics(
		"define test():\n"
		"    setting(RSK_FOO) is Setting.RO_BAR\n",
		"test");
	EXPECT_TRUE(result.diagnostics.empty());
	EXPECT_EQ(result.output, "True_(options=[OptionFilter(RSK_FOO, RO_BAR)])");
}

// `VALUE is setting(KEY)` means the same as `setting(KEY) is VALUE`, so it lowers the same.
TEST(ApOptionFilters, ReversedOperandOrderIsRecognized) {
	GenResult result = generateWithDiagnostics(
		"define test():\n"
		"    RO_BAR is setting(RSK_FOO)\n",
		"test");
	EXPECT_TRUE(result.diagnostics.empty());
	EXPECT_EQ(result.output, "True_(options=[OptionFilter(RSK_FOO, RO_BAR)])");
}

// A numeric setting compares against an int literal just as well as against an enum value.
TEST(ApOptionFilters, IntLiteralValueIsRecognized) {
	GenResult result = generateWithDiagnostics(
		"define test():\n"
		"    setting(RSK_FOO) is 3\n",
		"test");
	EXPECT_TRUE(result.diagnostics.empty());
	EXPECT_EQ(result.output, "True_(options=[OptionFilter(RSK_FOO, 3)])");
}

// An OptionFilter tests equality only, so an ordered comparison against a setting has no
// lowering -- diagnose rather than emit `True_(...) >= 3`, which raises at world-load.
TEST(ApOptionFilters, OrderedSettingComparisonIsDiagnosed) {
	GenResult result = generateWithDiagnostics(
		"define test():\n"
		"    setting(RSK_FOO) >= 3\n",
		"test");
	ASSERT_EQ(result.diagnostics.size(), 1u);
	EXPECT_EQ(result.diagnostics[0].level, rls::ast::DiagnosticLevel::Error);
	EXPECT_NE(result.diagnostics[0].message.find("equality"), std::string::npos)
		<< "message was: " << result.diagnostics[0].message;
}

// Two settings compared against each other is not an OptionFilter either: the filter needs a
// build-time value on the other side, and a rule there would compare Rule objects by identity.
TEST(ApOptionFilters, SettingComparedToSettingIsDiagnosed) {
	GenResult result = generateWithDiagnostics(
		"define test():\n"
		"    setting(RSK_FOO) is setting(RSK_BAZ)\n",
		"test");
	ASSERT_EQ(result.diagnostics.size(), 1u);
	EXPECT_EQ(result.diagnostics[0].level, rls::ast::DiagnosticLevel::Error);
}

// A bare setting(...) truthiness guard is a proper rule, so combining it with and/or is fine
// and must NOT trip the comparison diagnostic.
TEST(ApOptionFilters, SettingCombinedWithRuleIsNotDiagnosed) {
	GenResult result = generateWithDiagnostics(
		"define test():\n"
		"    setting(RSK_FOO) and has(RG_HOOKSHOT)\n",
		"test");
	EXPECT_TRUE(result.diagnostics.empty());
	EXPECT_EQ(result.output, "True_(options=[OptionFilter(RSK_FOO, True)]) & has(RG_HOOKSHOT)");
}

// Tests for "diagnose, don't miscompile" (§6.4 of docs/AP-Function-Generation.md): RLS
// that type-checks but cannot be expressed in the RuleBuilder target must raise a precise
// diagnostic rather than emit Python that throws at world-load. Covers negating a rule,
// a rule/runtime-valued ternary condition, and combining a runtime non-rule value with a
// rule. Generic AP behavior, tested against the minimal default-hook transpiler.
#include "helpers.h"

using namespace rls::transpilers::ap_tests;

namespace {
struct GenResult {
	std::string output;
	std::vector<rls::ast::Diagnostic> diagnostics;
};
} // namespace

static GenResult generate(const std::string& source, const std::string& defineName) {
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

// Assert exactly one error diagnostic whose message contains `needle`.
static void expectOneError(const GenResult& result, const std::string& needle) {
	ASSERT_EQ(result.diagnostics.size(), 1u);
	EXPECT_EQ(result.diagnostics[0].level, rls::ast::DiagnosticLevel::Error);
	EXPECT_NE(result.diagnostics[0].message.find(needle), std::string::npos)
		<< "message was: " << result.diagnostics[0].message;
}

// == not ======================================================================

// Negating a rule is unrepresentable (no rule negation in the RuleBuilder), so it is
// diagnosed rather than silently dropping the `not`.
TEST(ApDiagnostics, NegatingRuleIsDiagnosed) {
	expectOneError(generate(
		"define test():\n"
		"    not has(RG_HOOKSHOT)\n",
		"test"),
		"negate");
}

// `not setting(...)` is the one representable negation: it becomes an OptionFilter with a
// false value and raises no diagnostic.
TEST(ApDiagnostics, NegatingSettingIsNotDiagnosed) {
	GenResult result = generate(
		"define test():\n"
		"    not setting(RSK_FOO)\n",
		"test");
	EXPECT_TRUE(result.diagnostics.empty());
	EXPECT_EQ(result.output, "True_(options=[OptionFilter(RSK_FOO, False)])");
}

// Negating a build-time value is an ordinary Python `not` -- emitted, not dropped, and
// not diagnosed. (Regression guard for the previously silently-dropped `not`.)
TEST(ApDiagnostics, NegatingBuildTimeValueEmitsPythonNot) {
	GenResult result = generate(
		"define test(flag: Bool):\n"
		"    not flag\n",
		"test");
	EXPECT_TRUE(result.diagnostics.empty());
	EXPECT_EQ(result.output, "not flag");
}

// == ternary condition ========================================================

// A rule-conditioned ternary with *rule* branches is not diagnosed: it lowers to a
// conditional host rule (see ternary_tests.cpp). A rule-conditioned ternary with *value*
// branches fed into a rule-producing call is also representable -- it distributes into a
// conditional rule (also ternary_tests.cpp). What remains unrepresentable is a rule-conditioned
// ternary whose value is *itself* the result (below): there is no rule-producing call to lift.

// A standalone value-producing ternary whose condition is a rule (wallet_capacity style) cannot
// produce a state-dependent integer, and has no enclosing call to distribute over -- diagnosed.
TEST(ApDiagnostics, RuleConditionedValueTernaryIsDiagnosed) {
	expectOneError(generate(
		"define test():\n"
		"    has(RG_TYCOON_WALLET) ? 999 : 0\n",
		"test"),
		"ternary condition");
}

// The distribution only fires for a call that yields a Rule. A value-returning callee (price_of
// -> Int) is left alone: wrapping its non-rule results in a conditional rule would be a
// miscompile, so the rule-conditioned value ternary is diagnosed instead.
TEST(ApDiagnostics, ValueCallWithRuleConditionedTernaryArgIsDiagnosed) {
	expectOneError(generate(
		"extern define price_of(item: Item) -> Int\n"
		"define test():\n"
		"    price_of(has(RG_CLIMB) ? RG_HOOKSHOT : RG_LONGSHOT)\n",
		"test"),
		"ternary condition");
}

// Only one ternary argument per call is distributed (no upstream case needs more). A second
// rule-conditioned value ternary in the same call is safely diagnosed rather than miscompiled.
TEST(ApDiagnostics, SecondTernaryArgInOneCallIsDiagnosed) {
	GenResult result = generate(
		"extern define f(a: Item, b: Item) -> Bool\n"
		"define test():\n"
		"    f(has(RG_CLIMB) ? RG_HOOKSHOT : RG_LONGSHOT, has(RG_BOW) ? RG_ARROWS : RG_BOMBS)\n",
		"test");
	EXPECT_FALSE(result.diagnostics.empty());
}

// A build-time ternary condition is fine: the value is frozen when the rule is built, so
// it selects between the rule branches without a diagnostic.
TEST(ApDiagnostics, BuildTimeConditionedTernaryIsNotDiagnosed) {
	GenResult result = generate(
		"define test(pick: Bool):\n"
		"    pick ? has(RG_HOOKSHOT) : has(RG_BOW)\n",
		"test");
	EXPECT_TRUE(result.diagnostics.empty());
}

// == runtime value in and/or ==================================================

// A comparison over a runtime quantity (bottle_count) is neither a rule nor a build-time
// value; combining it with a rule via `and` is diagnosed.
TEST(ApDiagnostics, RuntimeValueCombinedWithRuleIsDiagnosed) {
	expectOneError(generate(
		"extern define bottle_count() -> Int\n"
		"define test():\n"
		"    bottle_count() >= 1 and has(RG_HOOKSHOT)\n",
		"test"),
		"runtime value");
}

// A match whose arms produce a runtime value (not a rule, not build-time) is unrepresentable.
TEST(ApDiagnostics, RuntimeValuedMatchIsDiagnosed) {
	expectOneError(generate(
		"extern define bottle_count() -> Int\n"
		"define test(d: Distance):\n"
		"    match d {\n"
		"        ED_CLOSE: bottle_count() >= 1\n"
		"    }\n",
		"test"),
		"runtime value");
}

// == Runtime values at the top of a rule ======================================
// The operator paths (and/or, ternary conditions) diagnose runtime operands as they combine
// them. A runtime value standing ALONE as a region entry condition or a function body has no
// such combining operator above it, so the whole-file generation paths check it there. Without
// that check the emitted `lambda bundle: bottle_count(bundle) >= 3` is evaluated once, against
// the empty initial collection state -- a silent miscompile, the exact thing §6.4 forbids.

// Run the function-definitions path and hand back the diagnostics it raised.
static std::vector<rls::ast::Diagnostic> generateFunctions(const std::string& source) {
	auto project = resolveFromSource(source);
	TestApTranspiler transpiler(project);
	MemoryWriter writer;
	transpiler.GenerateFunctionDefinitionsSource(writer);
	return transpiler.Diagnostics();
}

// Run the regions path and hand back the diagnostics it raised.
static std::vector<rls::ast::Diagnostic> generateRegions(const std::string& source) {
	auto project = resolveFromSource(source);
	TestApTranspiler transpiler(project);
	MemoryWriter writer;
	transpiler.GenerateRegionsSource(writer);
	return transpiler.Diagnostics();
}

TEST(ApDiagnostics, RuntimeValueAsFunctionBodyIsDiagnosed) {
	auto diagnostics = generateFunctions(
		"extern define bottle_count() -> Int\n"
		"define test():\n"
		"    bottle_count() >= 2\n");
	ASSERT_EQ(diagnostics.size(), 1u);
	EXPECT_EQ(diagnostics[0].level, rls::ast::DiagnosticLevel::Error);
	EXPECT_NE(diagnostics[0].message.find("runtime value"), std::string::npos)
		<< "message was: " << diagnostics[0].message;
}

TEST(ApDiagnostics, RuntimeValueAsEntryConditionIsDiagnosed) {
	auto diagnostics = generateRegions(
		"extern define bottle_count() -> Int\n"
		"region RR_FOO {\n"
		"    name: \"Foo\"\n"
		"    locations {\n"
		"        RC_BAR: bottle_count() >= 3\n"
		"    }\n"
		"}\n");
	ASSERT_EQ(diagnostics.size(), 1u);
	EXPECT_EQ(diagnostics[0].level, rls::ast::DiagnosticLevel::Error);
	EXPECT_NE(diagnostics[0].message.find("runtime value"), std::string::npos)
		<< "message was: " << diagnostics[0].message;
}

// A rule body and a build-time value body are both fine at the top of a rule -- only the
// runtime class is unrepresentable there.
TEST(ApDiagnostics, RuleAndBuildTimeBodiesAreNotDiagnosed) {
	EXPECT_TRUE(generateFunctions(
		"define rule_body():\n"
		"    has(RG_HOOKSHOT)\n"
		"define value_body(n: Int):\n"
		"    n + 1\n").empty());
}

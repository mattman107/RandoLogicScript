## Plan: RLS User And Extern Enum Support

Add first-class enum declarations to RLS with two forms: enum (project-owned) and extern enum (host-owned), support extern glob expansion from host registry, allow implicit enum/int conversion both ways, and enforce hard ambiguity errors requiring EnumName.ValueName syntax. Keep existing full constants usable by default, only requiring dot syntax when multiple enums expose the same value.

**Execution Checklist**

**Phase 1 - Type System Foundation**
- [x] Add enum-aware semantic typing foundation in [ast/include/ast.h](ast/include/ast.h).
- [x] Add Type::Enum.
- [x] Add enum identity side-table in Project (setEnumType/getEnumType).
- [x] Add enum metadata registry in Project (registerEnum/getEnumInfo).
- [x] Support unknown values for value-dependent warnings (`EnumMemberInfo.value: optional<int>`).
- [x] Implement option 1 model split: explicit enum members vs pattern declarations (`EnumEntryInfo = variant<EnumMemberInfo, EnumPatternInfo>`).
- [x] Wire Enum through sema helper utilities in [sema/src/type_helpers.h](sema/src/type_helpers.h).
- [x] Add non-breaking enum identity scaffolding in [sema/src/resolve_types.cpp](sema/src/resolve_types.cpp) for builtin enum identifiers.
- [ ] Add compatibility touchups in transpiler type routing for Type::Enum.
- [x] Add/adjust AST and sema tests for completed Phase 1 slices.

**Phase 2 - Grammar, AST, And Parsing**
- [x] Add top-level declarations enum and extern enum to grammar declaration alternatives, reserved keywords, parse-tree selector, and builder dispatch.
- [x] Add member-access expression syntax for EnumName.ValueName and AST node MemberExpr (or EnumValueRef equivalent) to represent dotted disambiguation explicitly.
- [x] Add enum member syntax supporting optional explicit integer assignment, with auto-increment for omitted values.
- [x] Add extern enum member entries supporting explicit names and glob pattern entries.
- [x] Add parser diagnostics for new keyword expectations and malformed enum declarations.

**Phase 3 - Declaration Collection And Validation**
- [x] Extend collectDeclarations to gather EnumDecl and ExternEnumDecl into global lookup maps and detect duplicate enum names.
- [x] Validate enum member rules.
- [x] Normal enum: no wildcard members allowed; duplicate member names forbidden; computed integer values unique.
- [x] Extern enum: explicit members and glob patterns allowed; at least one member source (explicit or wildcard) required.
- [x] Expand extern glob patterns against host enum registry (randomizer enum metadata), then materialize concrete members and detect overlaps/duplicates after expansion. *(Approach: glob patterns are matched at identifier resolution time in Phase 4 rather than materialised here; sibling overlap is checked at validation time.)*
- [x] Validate collisions between enum value names across enums are permitted but marked as potentially ambiguous for use-site resolution.

**Phase 4 - Identifier And Expression Type Resolution**
- [x] Replace prefix-only typeFromIdentifier behavior with two-stage lookup.
- [x] Stage A: exact enum value match across collected enum registries (builtin + normal + extern expanded).
- [x] Stage B: fallback prefix map for legacy/builtin values if not otherwise resolved.
- [ ] On bare identifier with multiple enum matches, emit hard error requiring EnumName.ValueName.
- [ ] Resolve MemberExpr by validating left side as enum type and right side as member of that enum; produce enum-typed result with enum identity set.
- [ ] Add implicit conversion rules requested.
- [ ] enum to int allowed in arithmetic/comparison/call binding.
- [ ] int to enum allowed where enum expected; if multiple enum members share the integer value across candidate enums, require explicit enum context or MemberExpr.
- [ ] Update call argument compatibility so enum identity is enforced for enum-typed parameters (same enum required unless explicit int conversion path is taken).
- [ ] Update match typing so discriminant/pattern unification supports enum identities and detects ambiguous bare values.

**Phase 5 - Transpiler Integration**
- [ ] Update SOH expression generation to emit qualified values for enum identifiers and MemberExpr using enum metadata instead of only ast::Type switch.
- [ ] Preserve existing built-in mappings (RandomizerGet::, RandomizerEnemy::, etc.) while allowing externally-mapped enum namespaces from registry metadata.
- [ ] Update function signature generation for enum-typed params/returns so generated C++ uses mapped host enum types.
- [ ] Ensure conversion behavior compiles cleanly by emitting explicit static_cast where required by C++ overload resolution.

**Phase 6 - Tests And Documentation**
- [ ] Parser tests: enum and extern enum declarations, optional explicit values, glob entries, dotted member expressions, malformed cases.
- [ ] Sema tests: duplicate enum declarations, member value assignment auto-increment, wildcard expansion, ambiguity diagnostics, enum/int implicit conversions, dot disambiguation success paths.
- [ ] AST tests: new declaration variants, enum identity table storage/retrieval, member expression node construction.
- [ ] Transpiler tests: generated C++ for bare and dotted enum values, enum params, conversion-heavy expressions.
- [ ] Docs updates: language spec sections on core types, type inference, enum declarations, extern wildcards, ambiguity rules, and conversion semantics.

**Phase 7 - Verification And Rollout**
- [ ] Run parser, sema, ast, and soh transpiler test suites.
- [ ] Add golden examples showing both non-ambiguous bare constants and ambiguity requiring EnumName.ValueName.
- [ ] Validate existing scripts still compile unchanged unless an intentional ambiguity is introduced by new enums.

**History (condensed)**
- 2026-06-25: Completed Phase 1 slices 1-5 (AST enum foundation, optional values, option 1 entry split, sema helper wiring, builtin enum identity scaffolding).
- 2026-06-25: Completed Phase 2 slice for enum/extern enum declarations (grammar + parse-tree selector + AST/builder wiring), added parser tests, and validated parser test suite.
- 2026-06-26: Completed Phase 2 slice for dotted enum member-access parsing (`EnumName.ValueName`) with malformed-case parser tests and full-suite validation.
- 2026-07-20: Completed Phase 3 collection/validation slice for enum declarations, enum value assignment checks, extern enum entry rules, and cross-enum ambiguity warnings.
- 2026-07-21: Completed Phase 4 Stage A+B (two-stage identifier lookup): implemented Stage A exact enum member matching (including glob patterns for extern enums), Stage B prefix map fallback, and ambiguity detection with hard error reporting. Added 5 comprehensive tests validating all lookup paths. All 643 tests passing.

**Steps**
1. Phase 1 - Type System Foundation (blocks all other phases)
A. Introduce enum-aware semantic typing without breaking existing primitive/builtin flow in ast::Type.
B. Add ast::Type::Enum and enum identity side-table in Project (for Expr and Param nodes) so two enum-typed expressions can still be checked for same enum type, not just same ast::Type bucket.
C. Add EnumInfo/EnumMember metadata registry in Project to store: enum name, kind (normal/extern), underlying type (Int for now), member list, integer values, source spans, and whether members came from explicit declarations or expanded patterns.
D. Dependency: this must land before parser/sema checks that compare enum identities.

2. Phase 2 - Grammar, AST, And Parsing (depends on 1 for final node wiring; parser work can start in parallel)
A. Add top-level declarations enum and extern enum to grammar declaration alternatives, reserved keywords, parse-tree selector, and builder dispatch.
B. Add member-access expression syntax for EnumName.ValueName and AST node MemberExpr (or EnumValueRef equivalent) to represent dotted disambiguation explicitly.
C. Add enum member syntax supporting optional explicit integer assignment, with auto-increment for omitted values.
D. Add extern enum member entries supporting explicit names and glob pattern entries.
E. Add parser diagnostics for new keyword expectations and malformed enum declarations.

3. Phase 3 - Declaration Collection And Validation (depends on 2)
A. Extend collectDeclarations to gather EnumDecl and ExternEnumDecl into global lookup maps and detect duplicate enum names.
B. Validate enum member rules:
C. Normal enum: no wildcard members allowed; duplicate member names forbidden; computed integer values unique.
D. Extern enum: explicit members and glob patterns allowed; at least one member source (explicit or wildcard) required.
E. Expand extern glob patterns against host enum registry (randomizer enum metadata), then materialize concrete members and detect overlaps/duplicates after expansion.
F. Validate collisions between enum value names across enums are permitted but marked as potentially ambiguous for use-site resolution.

4. Phase 4 - Identifier And Expression Type Resolution (depends on 1 and 3)
A. ✓ Replace prefix-only typeFromIdentifier behavior with two-stage lookup:
B. ✓ Stage A: exact enum value match across collected enum registries (builtin + normal + extern expanded).
C. ✓ Stage B: fallback prefix map for legacy/builtin values if not otherwise resolved.
D. On bare identifier with multiple enum matches, emit hard error requiring EnumName.ValueName.
E. Resolve MemberExpr by validating left side as enum type and right side as member of that enum; produce enum-typed result with enum identity set.
F. Add implicit conversion rules requested:
G. enum to int allowed in arithmetic/comparison/call binding.
H. int to enum allowed where enum expected; if multiple enum members share the integer value across candidate enums, require explicit enum context or MemberExpr.
I. Update call argument compatibility so enum identity is enforced for enum-typed parameters (same enum required unless explicit int conversion path is taken).
J. Update match typing so discriminant/pattern unification supports enum identities and detects ambiguous bare values.

5. Phase 5 - Transpiler Integration (depends on 4)
A. Update SOH expression generation to emit qualified values for enum identifiers and MemberExpr using enum metadata instead of only ast::Type switch.
B. Preserve existing built-in mappings (RandomizerGet::, RandomizerEnemy::, etc.) while allowing externally-mapped enum namespaces from registry metadata.
C. Update function signature generation for enum-typed params/returns so generated C++ uses mapped host enum types.
D. Ensure conversion behavior compiles cleanly by emitting explicit static_cast where required by C++ overload resolution.

6. Phase 6 - Tests And Documentation (parallel with 3-5 once interfaces stabilize)
A. Parser tests: enum and extern enum declarations, optional explicit values, glob entries, dotted member expressions, malformed cases.
B. Sema tests: duplicate enum declarations, member value assignment auto-increment, wildcard expansion, ambiguity diagnostics, enum/int implicit conversions, dot disambiguation success paths.
C. AST tests: new declaration variants, enum identity table storage/retrieval, member expression node construction.
D. Transpiler tests: generated C++ for bare and dotted enum values, enum params, conversion-heavy expressions.
E. Docs updates: language spec sections on core types, type inference, enum declarations, extern wildcards, ambiguity rules, and conversion semantics.

7. Phase 7 - Verification And Rollout (depends on all prior phases)
A. Run parser, sema, ast, and soh transpiler test suites.
B. Add golden examples showing both non-ambiguous bare constants and ambiguity requiring EnumName.ValueName.
C. Validate existing scripts still compile unchanged unless an intentional ambiguity is introduced by new enums.

**Relevant files**
- c:/Users/Anthony/source/repos/RandoLogicScript/ast/include/ast.h - add EnumDecl/ExternEnumDecl, enum member structures, MemberExpr, Type and Project metadata maps.
- c:/Users/Anthony/source/repos/RandoLogicScript/parser/src/grammar.h - add enum syntax, extern enum syntax, member access dot grammar, member assignment, wildcard token rules.
- c:/Users/Anthony/source/repos/RandoLogicScript/parser/src/builder.h - parse-tree selector entries for new declaration and expression nodes.
- c:/Users/Anthony/source/repos/RandoLogicScript/parser/src/builder.cpp - CST to AST builders for enum decls, extern enum decls, member expressions, and enum members.
- c:/Users/Anthony/source/repos/RandoLogicScript/parser/src/parser.cpp - parse error messages for enum constructs.
- c:/Users/Anthony/source/repos/RandoLogicScript/sema/src/collect_declarations.h - update pass description to include enum declarations.
- c:/Users/Anthony/source/repos/RandoLogicScript/sema/src/collect_declarations.cpp - collect enum declarations and duplicate diagnostics.
- c:/Users/Anthony/source/repos/RandoLogicScript/sema/src/resolve_types.h - adjust typeFromIdentifier contract for project-aware enum lookup.
- c:/Users/Anthony/source/repos/RandoLogicScript/sema/src/resolve_types.cpp - enum-aware identifier resolution, ambiguity checks, conversion rules, member expression typing.
- c:/Users/Anthony/source/repos/RandoLogicScript/sema/src/type_helpers.h - enum-aware type naming and compatibility helpers.
- c:/Users/Anthony/source/repos/RandoLogicScript/sema/src/validate_declarations.cpp - enum declaration semantic validation and extern wildcard expansion checks.
- c:/Users/Anthony/source/repos/RandoLogicScript/transpilers/soh/src/generate_expression.cpp - enum/member expression emission and namespace mapping from registry metadata.
- c:/Users/Anthony/source/repos/RandoLogicScript/transpilers/soh/src/generate_functions.cpp - enum parameter and return type mapping.
- c:/Users/Anthony/source/repos/RandoLogicScript/parser/tests/parser_tests.cpp - parser coverage for new syntax.
- c:/Users/Anthony/source/repos/RandoLogicScript/sema/tests/resolve_types_tests.cpp - type resolution and ambiguity/conversion tests.
- c:/Users/Anthony/source/repos/RandoLogicScript/ast/tests/ast_tests.cpp - AST and Project type/enum metadata tests.
- c:/Users/Anthony/source/repos/RandoLogicScript/transpilers/soh/tests/generate_expression_test.cpp - expression emission tests.
- c:/Users/Anthony/source/repos/RandoLogicScript/transpilers/soh/tests/generate_functions_tests.cpp - function signature and default argument emission tests.
- c:/Users/Anthony/source/repos/RandoLogicScript/docs/RandoLogicScript-Full.md - language spec updates for enums, conversions, and disambiguation.

**Verification**
1. Configure and run full test suites for ast, parser, sema, and transpilers via CTest in the build directory.
2. Add targeted regression cases:
1. Bare full constant resolves when unique.
1. Same value in two enums fails without dot syntax.
1. EnumName.ValueName resolves successfully.
1. Extern wildcard expansion resolves host constants from registry.
1. int arithmetic and enum comparisons compile and type-check under implicit conversion rules.
3. Validate generated SOH output for representative scripts with new enum declarations and extern enum patterns.

**Decisions**
- Declaration syntax: enum Name { ... } and extern enum Name { explicit values and/or patterns }.
- Extern wildcard style: glob patterns (examples: RG_*, *_KEY, R*_BOSS).
- Raw value usage: keep existing full constants like RG_HOOKSHOT usable as bare identifiers.
- Ambiguity handling: hard error requiring EnumName.ValueName.
- Dot disambiguation style: always EnumName.ValueName, including host-prefixed values.
- Integer conversion: implicit enum to int and int to enum in semantic typing.
- Integer assignment in enums: optional explicit assignment with auto-increment fallback.
- Wildcard expansion source: host enum registry metadata.

**Scope boundaries**
- Included: parser/AST/sema/SOH transpiler/docs/tests needed for enum language support.
- Excluded for this change: broad AP transpiler parity work beyond compile-safe pass-through, IDE/LSP completion enhancements, and runtime interpreter behavior (RLS remains transpile-time only).

**Further Considerations**
1. Host registry contract should be frozen early (member name, integer value, C++ qualified type path) to prevent rework in Phase 5.
2. If int to enum implicit conversion causes noisy ambiguity in practice, add a follow-up lint mode before changing language semantics.
3. Introduce a compatibility flag only if existing scripts encounter unexpected ambiguity due to newly added user enums in shared projects.

**It is imperative to use the VS Code CMake Tools extension for configuring, building, and running tests, otherwise the project may not compile due to bitness issues.**
**Each step must be reviewed, be sure to take small steps to make it easier to review and commit.**
**Because this will make big changes to the type system, it may be necessary to disable unit tests temporarily to get the project to compile and pass the remaining tests. Be sure to re-enable and run all tests after each phase is complete.**
**Keep this plan up to date with any changes and progress to the implementation.**
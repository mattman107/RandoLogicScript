#include "validate_declarations.h"
#include "type_helpers.h"

#include <algorithm>
#include <format>
#include <queue>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace rls::sema {

static const char* sectionKindName(ast::SectionKind kind) {
	switch (kind) {
	case ast::SectionKind::Events:    return "event";
	case ast::SectionKind::Locations: return "location";
	case ast::SectionKind::Exits:     return "exit";
	}
	return "entry";
}

static bool globMatches(std::string_view pattern, std::string_view value) {
	// Simple '*' wildcard matcher (matches zero or more chars).
	size_t p = 0;
	size_t v = 0;
	size_t star = std::string_view::npos;
	size_t backtrack = 0;

	while (v < value.size()) {
		if (p < pattern.size() && pattern[p] == value[v]) {
			++p;
			++v;
			continue;
		}
		if (p < pattern.size() && pattern[p] == '*') {
			star = p++;
			backtrack = v;
			continue;
		}
		if (star != std::string_view::npos) {
			p = star + 1;
			v = ++backtrack;
			continue;
		}
		return false;
	}

	while (p < pattern.size() && pattern[p] == '*') {
		++p;
	}

	return p == pattern.size();
}

static void checkEnumDeclarations(
	ast::Project& project, std::vector<ast::Diagnostic>& diags)
{
	std::unordered_map<std::string, std::set<std::string>> valueNameToEnums;

	for (auto& [enumName, info] : project.EnumInfos) {
		if (info.kind == ast::EnumKind::Normal) {
			std::unordered_set<std::string> seenNames;
			std::unordered_set<int> seenValues;
			std::vector<ast::EnumEntryInfo> normalized;
			normalized.reserve(info.entries.size());

			int nextValue = 0;
			for (const auto& entry : info.entries) {
				if (std::holds_alternative<ast::EnumPatternInfo>(entry)) {
					const auto& pattern = std::get<ast::EnumPatternInfo>(entry);
					diags.push_back({
						ast::DiagnosticLevel::Error,
						std::format("enum '{}' cannot contain wildcard pattern '{}'", enumName, pattern.pattern),
						pattern.span
					});
					continue;
				}

				auto member = std::get<ast::EnumMemberInfo>(entry);
				if (!seenNames.insert(member.name.text).second) {
					diags.push_back({
						ast::DiagnosticLevel::Error,
						std::format("duplicate enum member '{}' in enum '{}'", member.name.text, enumName),
						member.span
					});
					continue;
				}

				if (member.value.has_value()) {
					nextValue = *member.value;
				} else {
					member.value = nextValue;
				}

				if (!seenValues.insert(*member.value).second) {
					diags.push_back({
						ast::DiagnosticLevel::Error,
						std::format("duplicate enum value {} in enum '{}'", *member.value, enumName),
						member.span
					});
				}

				nextValue = *member.value + 1;
				normalized.emplace_back(std::move(member));
			}

			info.entries = std::move(normalized);

			for (const auto& entry : info.entries) {
				if (std::holds_alternative<ast::EnumMemberInfo>(entry)) {
					const auto& member = std::get<ast::EnumMemberInfo>(entry);
					valueNameToEnums[member.name.text].insert(enumName);
				}
			}
			continue;
		}

		// Extern enums: explicit members and wildcard patterns are allowed.
		if (info.entries.empty()) {
			diags.push_back({
				ast::DiagnosticLevel::Error,
				std::format("extern enum '{}' must declare at least one member or wildcard pattern", enumName),
				info.span
			});
			continue;
		}

		std::unordered_set<std::string> explicitNames;
		std::vector<std::string> patterns;
		for (const auto& entry : info.entries) {
			if (std::holds_alternative<ast::EnumMemberInfo>(entry)) {
				const auto& member = std::get<ast::EnumMemberInfo>(entry);
				if (!explicitNames.insert(member.name.text).second) {
					diags.push_back({
						ast::DiagnosticLevel::Error,
						std::format("duplicate enum member '{}' in extern enum '{}'", member.name.text, enumName),
						member.span
					});
				}
				valueNameToEnums[member.name.text].insert(enumName);
			} else {
				const auto& pattern = std::get<ast::EnumPatternInfo>(entry);
				patterns.push_back(pattern.pattern);
			}
		}

		// Check wildcard overlap against explicit sibling members.
		// Full host-registry matching is deferred to Phase 4: the identifier
		// resolver calls globMatches(pattern, identifierName) at use-site rather
		// than materialising a concrete member list here.
		for (const auto& pattern : patterns) {
			for (const auto& explicitName : explicitNames) {
				if (globMatches(pattern, explicitName)) {
					diags.push_back({
						ast::DiagnosticLevel::Warning,
						std::format(
							"extern enum '{}' wildcard '{}' overlaps explicit member '{}'",
							enumName,
							pattern,
							explicitName),
						info.span
					});
				}
			}
		}
	}

	for (const auto& [valueName, enums] : valueNameToEnums) {
		if (enums.size() <= 1) {
			continue;
		}
		std::string enumList;
		bool first = true;
		for (const auto& enumName : enums) {
			if (!first) {
				enumList += ", ";
			}
			enumList += enumName;
			first = false;
		}

		diags.push_back({
			ast::DiagnosticLevel::Warning,
			std::format(
				"enum value '{}' appears in multiple enums ({}) and may require dotted disambiguation",
				valueName,
				enumList),
			{}
		});
	}
}

/// Check 1: Every extend-region must target a declared region.
static void checkExtendRegionTargets(
	ast::Project& project, std::vector<ast::Diagnostic>& diags)
{
	for (auto& [name, decls] : project.ExtendRegionDecls) {
		if (!project.RegionDecls.contains(name)) {
			for (const auto* decl : decls) {
				diags.push_back({
					ast::DiagnosticLevel::Error,
					std::format("extend region targets unknown region '{}'", name),
					decl->span
				});
			}
		}
	}
}

/// Check 2: No duplicate entries across base region + all its extensions
///           within the same SectionKind.
static void checkDuplicateEntries(
	ast::Project& project, std::vector<ast::Diagnostic>& diags)
{
	for (auto& [regionName, regionDecl] : project.RegionDecls) {
		std::unordered_map<ast::SectionKind, std::unordered_set<std::string>> seen;

		auto checkEntries = [&](const std::vector<ast::Section>& sections) {
			for (const auto& section : sections) {
				auto& set = seen[section.kind];
				for (const auto& entry : section.entries) {
					if (!set.insert(entry.name.text).second) {
						diags.push_back({
							ast::DiagnosticLevel::Error,
							std::format("duplicate {} '{}' in region '{}'",
								sectionKindName(section.kind), entry.name.text, regionName),
							entry.span
						});
					}
				}
			}
		};

		checkEntries(regionDecl->body.sections);

		if (auto it = project.ExtendRegionDecls.find(regionName);
			it != project.ExtendRegionDecls.end()) {
			for (const auto* decl : it->second) {
				checkEntries(decl->sections);
			}
		}
	}
}

/// Check 3: Entry conditions must be Bool-compatible.
static void checkEntryConditionTypes(
	ast::Project& project, std::vector<ast::Diagnostic>& diags)
{
	auto check = [&](const std::vector<ast::Section>& sections,
		const std::string& regionName) {
		for (const auto& section : sections) {
			for (const auto& entry : section.entries) {
				auto condType = project.getType(entry.condition.get());
				if (!condType || *condType == ast::Type::Error) continue;
				if (!isBoolCompatible(*condType)) {
					diags.push_back({
						ast::DiagnosticLevel::Error,
						std::format("{} condition for '{}' in region '{}' must be Bool, got {}",
							sectionKindName(section.kind), entry.name.text, regionName,
							typeName(*condType)),
						entry.span
					});
				}
			}
		}
	};

	for (auto& [name, decl] : project.RegionDecls) {
		check(decl->body.sections, name);
	}
	for (auto& [name, decls] : project.ExtendRegionDecls) {
		for (const auto* decl : decls) {
			check(decl->sections, name);
		}
	}
}

/// Check 4: Every region must be reachable from RR_ROOT via exits.
static void checkRegionReachability(
	ast::Project& project, std::vector<ast::Diagnostic>& diags)
{
	if (!project.RegionDecls.contains("RR_ROOT")) return;

	// Build directed graph: region → set of regions reachable via exits.
	std::unordered_map<std::string, std::unordered_set<std::string>> graph;

	for (auto& [regionName, decl] : project.RegionDecls) {
		auto& targets = graph[regionName];
		for (const auto& section : decl->body.sections) {
			if (section.kind == ast::SectionKind::Exits) {
				for (const auto& entry : section.entries) {
					targets.insert(entry.name.text);
				}
			}
		}
	}
	for (auto& [regionName, decls] : project.ExtendRegionDecls) {
		if (!project.RegionDecls.contains(regionName)) continue;
		auto& targets = graph[regionName];
		for (const auto* decl : decls) {
			for (const auto& section : decl->sections) {
				if (section.kind == ast::SectionKind::Exits) {
					for (const auto& entry : section.entries) {
						targets.insert(entry.name.text);
					}
				}
			}
		}
	}

	// BFS from RR_ROOT.
	std::unordered_set<std::string> visited;
	std::queue<std::string> frontier;
	frontier.push("RR_ROOT");
	visited.insert("RR_ROOT");

	while (!frontier.empty()) {
		auto current = frontier.front();
		frontier.pop();
		if (auto it = graph.find(current); it != graph.end()) {
			for (const auto& target : it->second) {
				if (visited.insert(target).second) {
					frontier.push(target);
				}
			}
		}
	}

	for (auto& [regionName, decl] : project.RegionDecls) {
		if (!visited.contains(regionName)) {
			diags.push_back({
				ast::DiagnosticLevel::Warning,
				std::format("region '{}' is not reachable from 'RR_ROOT'", regionName),
				decl->span
			});
		}
	}
}

/// Check 5: Every define should be referenced somewhere.
static void checkUnusedDefines(
	ast::Project& project, std::vector<ast::Diagnostic>& diags)
{
	if (project.DefineDecls.empty()) return;

	std::unordered_set<std::string> usedFunctions;

	// Walk all expression trees to collect function call names.
	auto collectFromSections = [&](const std::vector<ast::Section>& sections) {
		for (const auto& section : sections) {
			for (const auto& entry : section.entries) {
				collectCallNames(*entry.condition, usedFunctions);
			}
		}
	};

	for (auto& [name, decl] : project.RegionDecls) {
		collectFromSections(decl->body.sections);
	}
	for (auto& [name, decls] : project.ExtendRegionDecls) {
		for (const auto* decl : decls) {
			collectFromSections(decl->sections);
		}
	}
	for (auto& [name, decl] : project.DefineDecls) {
		collectCallNames(*decl->body, usedFunctions);
		for (const auto& param : decl->params) {
			if (param.defaultValue) {
				collectCallNames(*param.defaultValue, usedFunctions);
			}
		}
	}
	for (auto& [name, decl] : project.DefineDecls) {
		if (!usedFunctions.contains(name)) {
			diags.push_back({
				ast::DiagnosticLevel::Info,
				std::format("'{}' is defined but never used", name),
				decl->span
			});
		}
	}
}

/// Check 6: Define/extern signatures must have valid parameter shapes and
/// typed defaults must match annotated parameter types.
static void checkFunctionSignatures(
	ast::Project& project, std::vector<ast::Diagnostic>& diags)
{
	auto validateParams = [&](const std::string& kind,
		bool isExtern,
		const std::string& name,
		const std::vector<ast::Param>& params,
		const ast::Span& declSpan) {
		std::unordered_set<std::string> seenNames;
		bool seenDefault = false;

		for (const auto& param : params) {
			if (!seenNames.insert(param.name.text).second) {
				diags.push_back({
					ast::DiagnosticLevel::Error,
					std::format(
						"duplicate parameter '{}' in {} '{}'",
						param.name.text, kind, name),
					declSpan
				});
			}

			if (param.defaultValue) {
				seenDefault = true;
			} else if (seenDefault) {
				diags.push_back({
					ast::DiagnosticLevel::Error,
					std::format(
						"required parameter '{}' cannot follow optional parameters in {} '{}'",
						param.name.text, kind, name),
					declSpan
				});
			}

			if (isExtern && !param.type && !param.defaultValue) {
				diags.push_back({
					ast::DiagnosticLevel::Error,
					std::format(
						"extern define '{}' parameter '{}' must have a type annotation or a default value",
						name,
							param.name.text),
					declSpan
				});
				continue;
			}

			if (isExtern && !param.type && param.defaultValue) {
				auto defaultType = project.getType(param.defaultValue.get());
				if (!defaultType || *defaultType == ast::Type::Error) {
					diags.push_back({
						ast::DiagnosticLevel::Error,
						std::format(
							"extern define '{}' parameter '{}' needs an explicit type or an inferrable default",
							name,
							param.name.text),
						param.defaultValue->span
					});
				}
				continue;
			}

			if (!param.type || !param.defaultValue) continue;

			auto paramType = project.getType(&param);
			auto defaultType = project.getType(param.defaultValue.get());
			if (!paramType || !defaultType) continue;
			if (*paramType == ast::Type::Error || *defaultType == ast::Type::Error) {
				continue;
			}
			auto isDefaultCompatible = [&](ast::Type expected, ast::Type actual) {
				if (expected == ast::Type::Condition) {
					return actual == ast::Type::Condition || isBoolCompatible(actual);
				}
				if (expected == ast::Type::Callable) {
					return actual == ast::Type::Callable || actual == ast::Type::Condition || isBoolCompatible(actual);
				}
				return actual == expected;
			};

			if (!isDefaultCompatible(*paramType, *defaultType)) {
				diags.push_back({
					ast::DiagnosticLevel::Error,
					std::format(
						"default value for parameter '{}' in {} '{}' has type {}, expected {}",
						param.name.text,
						kind,
						name,
						typeName(*defaultType),
						typeName(*paramType)),
					param.defaultValue->span
				});
			}
		}
	};

	for (const auto& [name, decl] : project.DefineDecls) {
		validateParams("define", false, name, decl->params, decl->span);
	}
	for (const auto& [name, decl] : project.ExternDefineDecls) {
		validateParams("extern define", true, name, decl->params, decl->span);

		if (!decl->returnType) {
			diags.push_back({
				ast::DiagnosticLevel::Error,
				std::format("extern define '{}' must declare a return type", name),
				decl->span
			});
			continue;
		}

		if (!typeFromAnnotation(decl->returnType->name.text)) {
			diags.push_back({
				ast::DiagnosticLevel::Error,
				std::format(
					"unknown return type annotation '{}' for extern define '{}'",
					decl->returnType->name.text,
					name),
				decl->span
			});
		}
	}
}

std::vector<ast::Diagnostic> validateDeclarations(ast::Project& project) {
	std::vector<ast::Diagnostic> diags;

	checkExtendRegionTargets(project, diags);
	checkDuplicateEntries(project, diags);
	checkEntryConditionTypes(project, diags);
	checkRegionReachability(project, diags);
	checkUnusedDefines(project, diags);
	checkFunctionSignatures(project, diags);
	checkEnumDeclarations(project, diags);

	return diags;
}

} // namespace rls::sema

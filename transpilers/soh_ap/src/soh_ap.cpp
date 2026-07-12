#include "soh_ap.h"

namespace rls::transpilers::soh_ap {

SohApTranspiler::SohApTranspiler(const rls::ast::Project& project)
	: ap::ApTranspiler(project) {}

void SohApTranspiler::Transpile(rls::OutputWriter& out) const {
	GenerateRegionsSource(out);
	GenerateEnumsSource(out);
	GenerateFunctionDefinitionsSource(out);
}

std::string SohApTranspiler::ruleContextParam() const {
	return "bundle";
}

std::string SohApTranspiler::ruleContextOptions() const {
	// SoH's rule-context receiver is the bundle `(region, world)`, so options live at bundle[1].
	return ruleContextParam() + "[1].options";
}

void Transpile(const rls::ast::Project& project, rls::OutputWriter& out) {
	SohApTranspiler(project).Transpile(out);
}

} // namespace rls::transpilers::soh_ap

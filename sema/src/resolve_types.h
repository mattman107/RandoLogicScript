#pragma once

#include <vector>

#include "ast.h"

namespace rls::sema {

/// Pass 2: Resolve and check types for all expressions in the project.
/// Populates Project::TypeTable via setType().
///
/// Currently handles:
///   - Declared enum identifiers (Step 1)
///   - Call validation via enemy built-ins + extern define + define resolution (Step 2)
///   - Bottom-up expression typing for all node types (Step 3)
///   - Parameter scope for define and enemy field bodies (Step 4)
std::vector<ast::Diagnostic> resolveTypes(ast::Project& project);

} // namespace rls::sema

#pragma once

#include "compile/runner.hpp"

#include <array>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace apex::compile {

enum class Severity { Info, Warning, Error };
enum class ObjectKind { Brush, Entity, Side };

// A VMF id as a tool printed it. Ids are looked up in the document at click time (0.5 D5).
struct ObjectRef {
    ObjectKind kind;
    int id;
    bool operator==(const ObjectRef&) const = default;
};

struct Diagnostic {
    Severity severity = Severity::Warning;
    std::string tool;   // vbsp, vvis, vrad, ...
    std::string rule;   // stable name of the rule that matched, e.g. "vbsp.leak"
    std::string text;
    std::vector<ObjectRef> objects;
    std::optional<std::array<double, 3>> position; // where the tool named a point but no object
    std::size_t step = 0;
    bool operator==(const Diagnostic&) const = default;
};

struct ToolCounts {
    int errors = 0;
    int warnings = 0;
};
struct DiagnosticSummary {
    std::map<std::string, ToolCounts> tools;
    int seconds = 0; // the sum of the "N seconds elapsed" lines the tools print
};

// The tool a step ran: the executable's file name without directory or extension, lower-cased.
std::string toolName(const std::string& program);
// Rules are per tool and built from logs the real Linux tools wrote (tests/compile/logs).
std::vector<Diagnostic> parseLog(const std::string& tool, const std::vector<std::string>& lines);
std::vector<Diagnostic> collectDiagnostics(const std::vector<StepResult>& steps);
DiagnosticSummary summarize(const std::vector<Diagnostic>& diagnostics, const std::vector<StepResult>& steps);

} // namespace apex::compile

#include "compile/diagnostics.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <regex>

namespace apex::compile {
namespace {
std::string trimmed(std::string line) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    return line;
}
// A process that ended on a signal: the runners report -1 for a crash, and a shell reports 128 + N.
bool crashed(std::optional<int> code) {
    return code && (*code < 0 || (*code > 128 && *code < 128 + 65));
}
} // namespace

std::string toolName(const std::string& program) {
    auto name = std::filesystem::path(program).stem().string();
    std::ranges::transform(name, name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return name;
}

std::vector<Diagnostic> parseLog(const std::string& tool, const std::vector<std::string>& lines,
                                 std::optional<int> exitCode) {
    static const std::regex brush(R"(^Brush (\d+): (.+)$)");
    static const std::regex side(R"(^Side (\d+)$)");
    static const std::regex texture(R"(^Texture: (.+)$)");
    static const std::regex material(R"(^Material not found!: (.+)$)");
    static const std::regex model(R"re(^Error loading studio model "(.+)"!$)re");
    static const std::regex leakEntity(R"(^Entity (\S+) \(([-0-9.]+) ([-0-9.]+) ([-0-9.]+)\) leaked!$)");
    static const std::regex instance(R"(^Could not open instance file (.+)$)");
    static const std::regex portals(R"(^LoadPortals: couldn't read (.+)$)");
    std::vector<Diagnostic> found;
    // vbsp prints a brush error as "Brush N: reason", then "Side i" and "Texture: name" on later lines.
    Diagnostic* brushError = nullptr;
    Diagnostic* leak = nullptr;
    const auto add = [&](Severity severity, const char* rule, std::string text) -> Diagnostic& {
        found.push_back({severity, tool, rule, std::move(text), {}, std::nullopt, 0});
        brushError = leak = nullptr;
        return found.back();
    };
    std::string step; // vbsp's last step heading ("WriteBSP...")
    for (auto raw : lines) {
        const auto line = trimmed(std::move(raw));
        std::smatch m;
        if (line.ends_with("...")) step = line;
        if (tool == "vbsp") {
            if (std::regex_match(line, m, brush)) {
                auto& d = add(Severity::Error, "vbsp.brush", "Brush " + m[1].str() + ": " + m[2].str());
                d.objects.push_back({ObjectKind::Brush, std::stoi(m[1])});
                brushError = &d;
            } else if (brushError && std::regex_match(line, m, side)) {
                brushError->text += ", side " + m[1].str();
            } else if (brushError && std::regex_match(line, m, texture)) {
                brushError->text += ", texture " + m[1].str();
                brushError = nullptr;
            } else if (std::regex_match(line, m, material)) {
                add(Severity::Warning, "vbsp.material", "Material not found: " + m[1].str());
            } else if (std::regex_match(line, m, model)) {
                add(Severity::Warning, "vbsp.model", "Model failed to load: " + m[1].str());
            } else if (line == "**** leaked ****") {
                leak = &add(Severity::Error, "vbsp.leak", "The map leaks");
            } else if (std::regex_match(line, m, leakEntity)) {
                if (!leak) leak = &add(Severity::Error, "vbsp.leak", "The map leaks");
                leak->text = "The map leaks from " + m[1].str() + " at (" + m[2].str() + " " + m[3].str() + " " + m[4].str() + ")";
                leak->position = std::array{std::stod(m[2]), std::stod(m[3]), std::stod(m[4])};
                leak = nullptr;
            } else if (std::regex_match(line, m, instance)) {
                add(Severity::Error, "vbsp.instance", "Could not open instance file " + m[1].str());
            }
        } else if (tool == "vvis" && std::regex_match(line, m, portals)) {
            add(Severity::Error, "vvis.portals", "No portal file was written (" + m[1].str() + "); a leak stops vvis");
        }
    }
    // Plan 0.8 bite #8: in a map with displacements a leak can crash vbsp while it finds displacement
    // neighbours, before it writes the .lin file, so vbsp.leak never fires and the log just stops.
    if (tool == "vbsp" && crashed(exitCode) && step.starts_with("Finding displacement neighbors"))
        add(Severity::Error, "vbsp.displacement_crash",
            "vbsp crashed while finding displacement neighbors: the map probably leaks. Compile once with the "
            "displacements' solids hidden (or cordoned off) to find the leak");
    return found;
}

std::vector<Diagnostic> collectDiagnostics(const std::vector<StepResult>& steps) {
    std::vector<Diagnostic> all;
    for (std::size_t i = 0; i < steps.size(); ++i) {
        if (steps[i].program.empty()) continue;
        for (auto& d : parseLog(toolName(steps[i].program), steps[i].lines, steps[i].exitCode)) {
            d.step = i;
            all.push_back(std::move(d));
        }
    }
    return all;
}

DiagnosticSummary summarize(const std::vector<Diagnostic>& diagnostics, const std::vector<StepResult>& steps) {
    static const std::regex elapsed(R"(^(\d+) seconds elapsed\s*$)");
    DiagnosticSummary summary;
    for (const auto& d : diagnostics) {
        auto& counts = summary.tools[d.tool];
        if (d.severity == Severity::Error) ++counts.errors;
        else if (d.severity == Severity::Warning) ++counts.warnings;
    }
    for (const auto& step : steps)
        for (const auto& line : step.lines)
            if (std::smatch m; std::regex_match(line, m, elapsed)) summary.seconds += std::stoi(m[1]);
    return summary;
}

} // namespace apex::compile

#pragma once

#include "editor/asset_report.hpp"
#include "editor/instance_expansion.hpp"

#include <functional>
#include <string>
#include <vector>

namespace apex::editor {

// 0.5 D8: one list over checks that already exist. A problem lists and selects; it fixes nothing.
enum class ProblemSeverity { Error, Warning, Note }; // error: the compile is likely to fail
enum class ProblemKind {
    InvalidSolid, MissingAsset, BrokenConnection, UnknownClass, DuplicateName, InstanceProblem,
    OutOfRange, EmptyBrushEntity, MissingOrigin, MissingPlayerStart, EmptyCordon
};
struct Problem {
    ProblemSeverity severity = ProblemSeverity::Warning;
    ProblemKind kind = ProblemKind::InvalidSolid;
    std::string text;
    std::vector<document::NodeId> objects; // what "select" selects
    std::string fixId;                     // an offered action ("select"), never an automatic edit
};

// What the checks may read. Optional parts that are absent skip the checks that need them.
struct ProblemContext {
    const document::Document& doc;
    const Problems* assets = nullptr;                         // AssetReport::problems(), 0.4 D11 and D6
    const std::vector<InstanceProblem>* instances = nullptr;  // InstanceExpansion::problems()
    const assets::EntityClassTable* classes = nullptr;
    std::vector<std::string> playerStartClasses{"info_player_start"};
    double coordinateLimit = 65536.0; // BSP v25's range
};

using ProblemCheck = std::function<void(const ProblemContext&, std::vector<Problem>&)>;

class ProblemRegistry {
public:
    void add(std::string name, ProblemCheck check);
    const std::vector<std::string>& names() const { return names_; }
    // Errors first, then warnings and notes, each in registration order.
    std::vector<Problem> run(const ProblemContext& context) const;
    static ProblemRegistry builtin();
private:
    std::vector<std::string> names_;
    std::vector<ProblemCheck> checks_;
};

int countProblems(const std::vector<Problem>& problems, ProblemSeverity severity);
const char* severityName(ProblemSeverity severity);

} // namespace apex::editor

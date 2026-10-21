#include "vmf_geometry.hpp"

#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <sstream>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: apex_replane input.vmf output.vmf\n";
        return 2;
    }
    try {
        std::ifstream input(argv[1], std::ios::binary);
        if (!input) throw std::runtime_error("cannot open input");
        const std::string bytes{std::istreambuf_iterator<char>(input), {}};
        auto file = apex::vmf::parse(bytes);
        if (!file) file = apex::vmf::parse(bytes, {apex::kv1::EscapeMode::Legacy});
        if (!file) throw std::runtime_error(file.error().what());
        std::size_t solids = 0, planes = 0;
        apex::vmf_geometry::visitFile(*file, [&](apex::vmf::Solid& solid) {
            const auto sides = apex::vmf_geometry::sides(solid);
            // This experiment isolates plane -> vertices -> plane equivalence.
            // point_data stays byte-identical and is checked separately by the corpus test.
            const auto polyhedron = apex::geometry::ConvexPolyhedron::fromPlanes(
                apex::vmf_geometry::planes(sides));
            if (!polyhedron) {
                throw std::runtime_error("solid " + std::string(solid.id()->raw()) + ": " +
                                         polyhedron.error().front().message);
            }
            for (auto& side : solid.sides) {
                const auto id = apex::vmf_geometry::require(side.id()->asInt());
                const auto face = std::find_if(polyhedron->faces.begin(), polyhedron->faces.end(),
                    [&](const auto& f) { return f.sideId == static_cast<std::uint32_t>(id); });
                if (face == polyhedron->faces.end()) {
                    throw std::runtime_error("redundant side has no vertex face: " + std::to_string(id));
                }
                const auto points = polyhedron->sourcePlanePoints(*face);
                if (!points) throw std::runtime_error("cannot derive plane");
                // Avoid the VMF numeric setter's six-digit rounding for this D8 experiment.
                std::ostringstream text;
                text.imbue(std::locale::classic());
                text << std::setprecision(std::numeric_limits<double>::max_digits10);
                for (std::size_t i = 0; i < points->size(); ++i) {
                    if (i) text << ' ';
                    const auto p = (*points)[i];
                    text << '(' << p.x << ' ' << p.y << ' ' << p.z << ')';
                }
                const auto changed = side.plane()->set(text.str());
                if (!changed) throw std::runtime_error(changed.error().what());
                ++planes;
            }
            ++solids;
        });
        std::ofstream output(argv[2], std::ios::binary);
        output << apex::vmf::write(*file);
        if (!output) throw std::runtime_error("cannot write output");
        std::cout << "Re-planed " << solids << " solids, " << planes << " planes\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

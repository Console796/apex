// Stair: an Apex script plugin (docs/plugins/script-host.md).
//
// Move the cursor over the ground (z = 0) to place the stair, change "Steps" in the tool strip, click to
// create it. All steps are created in ONE apex.edit batch, so a single undo removes the whole stair.
// Escape forgets the placement. Everything the viewport shows is recomputed from the parameters on each
// visual() call: the script keeps no cached geometry.

const double RUN = 16;       // depth of one step (x)
const double RISE = 8;       // height of one step (z)
const double WIDTH = 64;     // width of the stair (y)
const double GRID = 8;       // the stair snaps to this grid
const int MIN_STEPS = 1;
const int MAX_STEPS = 64;
const string MATERIAL = "DEV/DEV_MEASUREGENERIC01";

// The six planes of an axis aligned box, as apex plane equations (outward normal, distance = n . point).
array<apex::Plane>@ box(double x0, double y0, double z0, double x1, double y1, double z1) {
    array<apex::Plane> planes;
    planes.insertLast(apex::Plane(apex::Vec3( 1, 0, 0),  x1));
    planes.insertLast(apex::Plane(apex::Vec3(-1, 0, 0), -x0));
    planes.insertLast(apex::Plane(apex::Vec3( 0, 1, 0),  y1));
    planes.insertLast(apex::Plane(apex::Vec3( 0,-1, 0), -y0));
    planes.insertLast(apex::Plane(apex::Vec3( 0, 0, 1),  z1));
    planes.insertLast(apex::Plane(apex::Vec3( 0, 0,-1), -z0));
    return planes;
}

// Step i spans x [i*RUN, (i+1)*RUN) and rises to (i+1)*RISE, so the stair climbs towards +x.
array<apex::Plane>@ stepPlanes(const apex::Vec3 &in at, int i) {
    return box(at.x + i * RUN, at.y, at.z, at.x + (i + 1) * RUN, at.y + WIDTH, at.z + (i + 1) * RISE);
}

class StairTool {
    int steps = 8;
    bool placed = false;
    uint64 source = 0;
    apex::Vec3 at;

    double snap(double v) { return floor(v / GRID + 0.5) * GRID; }

    bool input(apex::Event@ ev, apex::View@ view) {
        if (ev.kind == apex::InputKind::KeyDown && ev.key == "Escape") { cancel(); return true; }
        if (ev.kind != apex::InputKind::Move && ev.kind != apex::InputKind::Press) return false;
        if (abs(view.rayDirection.z) < 1e-9) return false;
        double t = -view.rayOrigin.z / view.rayDirection.z;
        if (t < 0) return false;
        at = apex::Vec3(snap(view.rayOrigin.x + t * view.rayDirection.x),
                        snap(view.rayOrigin.y + t * view.rayDirection.y), 0);
        placed = true;
        source = ev.source;
        if (ev.kind == apex::InputKind::Press && ev.button == apex::Button::Left) return commit();
        return false;
    }

    void visual(uint64 src, apex::Visual@ v) {
        if (!placed || src != source) return;
        for (int i = 0; i < steps; i++) v.ghost(stepPlanes(at, i), MATERIAL, 0.35);
        // Footprint outline on the ground.
        apex::Vec3 a = at, b = at + apex::Vec3(steps * RUN, 0, 0);
        apex::Vec3 c = b + apex::Vec3(0, WIDTH, 0), d = at + apex::Vec3(0, WIDTH, 0);
        v.line(a, b, 1, 0.65, 0.15, 1, 2);
        v.line(b, c, 1, 0.65, 0.15, 1, 2);
        v.line(c, d, 1, 0.65, 0.15, 1, 2);
        v.line(d, a, 1, 0.65, 0.15, 1, 2);
    }

    void readout(apex::Readout@ r) {
        r.detail("" + steps + " steps, " + (steps * RISE) + " units high");
        r.status(placed ? "Click to place the stair" : "Move over the ground to place the stair");
        r.parameter("steps", "Steps", apex::ParamKind::Integer, steps, MIN_STEPS, MAX_STEPS);
    }

    bool setParameter(const string &in id, double value) {
        if (id != "steps") return false;
        steps = int(floor(value + 0.5));
        if (steps < MIN_STEPS) steps = MIN_STEPS;
        if (steps > MAX_STEPS) steps = MAX_STEPS;
        return true;
    }

    bool commit() {
        if (!placed || !apex::documentOpen()) return false;
        uint64 world = apex::root(0);
        if (world == 0) return false;
        apex::Edit@ edit = apex::Edit();
        for (int i = 0; i < steps; i++) edit.createSolid(world, stepPlanes(at, i), MATERIAL);
        return edit.commit("Create stair");
    }

    void cancel() { placed = false; }
    void forget(uint64 src) { if (src == source) placed = false; }
}

void main() {
    apex::registerTool("tool", "Stair", "Create a straight stair with a chosen number of steps", "StairTool");
}

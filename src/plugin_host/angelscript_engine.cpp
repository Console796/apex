#include "plugin_host/angelscript_engine.hpp"

#if defined(APEX_HAVE_ANGELSCRIPT)
#include <apex/plugin.hpp>
#include <angelscript.h>
#include <scriptarray/scriptarray.h>
#include <scriptmath/scriptmath.h>
#include <scriptstdstring/scriptstdstring.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#endif

namespace apex::plugin_host {
#if defined(APEX_HAVE_ANGELSCRIPT)
namespace {
using Clock = std::chrono::steady_clock;
using std::string;
constexpr asPWORD kUserData = 0x41505831;                       // "APX1": the State pointer on each asIScriptEngine
constexpr auto kLoadBudget = std::chrono::milliseconds(5000);   // compile-free budget for the registration entry point
constexpr auto kCallBudget = std::chrono::milliseconds(250);    // one callback into a script; a loop that long is a hang
constexpr asUINT kLineStride = 1024;                            // the clock is read every this many script lines
std::atomic<std::size_t> liveEngines{0};

// ---- value types shared with scripts (plain data, copied by value) -------------------------------------------
struct Vec3 { double x, y, z; };
struct Plane { Vec3 normal; double distance; };
void vecInit(Vec3* v) { *v = {0, 0, 0}; }
void vecInit3(double x, double y, double z, Vec3* v) { *v = {x, y, z}; }
Vec3 vecAdd(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 vecSub(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 vecScale(const Vec3& a, double s) { return {a.x * s, a.y * s, a.z * s}; }
Vec3 vecNeg(const Vec3& a) { return {-a.x, -a.y, -a.z}; }
double vecDot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 vecCross(const Vec3& a, const Vec3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
double vecLength(const Vec3& a) { return std::sqrt(vecDot(a, a)); }
Vec3 vecNormalized(const Vec3& a) { const double n = vecLength(a); return n > 0 ? vecScale(a, 1 / n) : Vec3{0, 0, 0}; }
void planeInit(Plane* p) { *p = {{0, 0, 0}, 0}; }
void planeInit2(const Vec3& n, double d, Plane* p) { *p = {n, d}; }
apex_vec3 toC(const Vec3& v) { return {v.x, v.y, v.z}; }
Vec3 fromC(const apex_vec3& v) { return {v.x, v.y, v.z}; }

// ---- engine state -------------------------------------------------------------------------------------------
struct State;
struct ToolType;
struct Instance;
struct ActionReg { State* state; asIScriptFunction* function; };

struct State {
    asIScriptEngine* engine = nullptr;
    asIScriptModule* module = nullptr;
    const apex_host* host = nullptr;
    const apex_document_v1* document = nullptr;
    const apex_edit_v1* edit = nullptr;
    const apex_selection_v1* selection = nullptr;
    string pluginId, section, messages, lastLogged, lastError;
    bool loading = false;
    asITypeInfo* stringArray = nullptr;
    asITypeInfo* idArray = nullptr;
    asIScriptFunction* onChanged = nullptr;
    std::vector<std::unique_ptr<ToolType>> tools;
    std::vector<std::unique_ptr<ActionReg>> actions;
    std::set<Instance*> instances;
    std::weak_ptr<State> weak;   // set by the engine that owns this state; tool instances keep it alive

    ~State();   // defined after ToolType: Clang instantiates the member destructors here otherwise
    // Drops every reference the engine holds and destroys it. Idempotent; tool instances that outlive this
    // (they hold the State alive) are left inert: their script objects are released here.
    void shutdown();
    void log(const string& text) const {
        if (!host || !host->log) return;
        const string line = "[" + pluginId + "] " + text;
        host->log(host->context, 0, {line.data(), line.size()});
    }
    // A script error: the log gets it once per distinct message (a broken visual() would otherwise repeat
    // every frame), and it is kept as the last error the engine saw.
    void fail(const string& text) {
        lastError = text;
        if (text == lastLogged) return;
        lastLogged = text;
        log("error: " + text);
    }
};
State* stateOf(asIScriptEngine* engine) { return engine ? static_cast<State*>(engine->GetUserData(kUserData)) : nullptr; }
State* active() {
    auto* ctx = asGetActiveContext();
    return ctx ? stateOf(ctx->GetEngine()) : nullptr;
}
void raise(const char* what) { if (auto* ctx = asGetActiveContext()) ctx->SetException(what); }
apex_str cstr(const string& s) { return {s.data(), s.size()}; }
string fromC(apex_str s) { return s.data ? string(s.data, s.size) : string{}; }

// ---- running script code -------------------------------------------------------------------------------------
struct Watch { Clock::time_point deadline; asUINT counter = 0; bool tripped = false; };
void lineCallback(asIScriptContext* ctx, Watch* watch) {
    if (++watch->counter % kLineStride != 0 || watch->tripped || Clock::now() < watch->deadline) return;
    watch->tripped = true;
    ctx->Abort();   // SetException is only legal inside a system function call, not from a line callback
}
string describeException(asIScriptContext* ctx) {
    std::ostringstream out;
    const char* section = nullptr;
    int column = 0;
    const int line = ctx->GetExceptionLineNumber(&column, &section);
    const auto* function = ctx->GetExceptionFunction();
    out << (section ? section : "script") << ':' << line << ':' << column << ": exception: "
        << (ctx->GetExceptionString() ? ctx->GetExceptionString() : "?");
    if (function) out << " (in " << function->GetDeclaration(true, true, false) << ')';
    return out.str();
}
// Runs `function` (with `self` as its object for a method). `bind` sets the arguments; `take` reads the
// result after a clean finish. Returns false after reporting when the script raised an exception or the
// context could not be prepared. Safe to nest: every call has its own context from the engine's pool.
template <class Bind, class Take>
bool run(State& state, asIScriptFunction* function, asIScriptObject* self, std::chrono::milliseconds budget, Bind&& bind, Take&& take) {
    if (!state.engine || !function) return false;
    asIScriptContext* ctx = state.engine->RequestContext();
    if (!ctx) { state.fail("no script context available"); return false; }
    Watch watch{Clock::now() + budget};
    struct Give {
        asIScriptEngine* engine; asIScriptContext* ctx;
        ~Give() { ctx->ClearLineCallback(); engine->ReturnContext(ctx); }
    } give{state.engine, ctx};
    if (ctx->Prepare(function) < 0) { state.fail(string("cannot prepare ") + function->GetDeclaration()); return false; }
    if (self) ctx->SetObject(self);
    bind(ctx);
    ctx->SetLineCallback(asFUNCTION(lineCallback), &watch, asCALL_CDECL);
    const int status = ctx->Execute();
    if (status == asEXECUTION_FINISHED) { take(ctx); return true; }
    if (status == asEXECUTION_EXCEPTION) state.fail(describeException(ctx));
    else if (status == asEXECUTION_ABORTED && watch.tripped) {
        int column = 0;
        const char* section = nullptr;
        const int line = ctx->GetLineNumber(0, &column, &section);
        state.fail(string(section ? section : "script") + ':' + std::to_string(line) + ':' + std::to_string(column) +
                   ": exception: script exceeded its time budget of " + std::to_string(budget.count()) + " ms (endless loop?) in " +
                   function->GetDeclaration(true, true, false));
    }
    else state.fail(string("script stopped (status ") + std::to_string(status) + ") in " + function->GetDeclaration());
    return false;
}
const auto noBind = [](asIScriptContext*) {};
const auto noTake = [](asIScriptContext*) {};

// ---- reference types the script holds handles to ----------------------------------------------------------
struct Event {
    int refs = 1;
    uint32_t kind = 0, button = 0, modifiers = 0, flags = 0;
    uint64_t source = 0;
    double x = 0, y = 0;
    string key;
    void addRef() { ++refs; }
    void release() { if (--refs == 0) delete this; }
};
struct View {
    int refs = 1;
    Vec3 rayOrigin{}, rayDirection{}, cameraPosition{}, cameraAngles{}, cameraForward{}, cameraUp{};
    double width = 0, height = 0, fovY = 0, orthoHeight = 0;
    bool orthographic = false;
    const apex_tool_view* live = nullptr;   // the host's view; valid only while the input callback runs
    void addRef() { ++refs; }
    void release() { if (--refs == 0) delete this; }
    bool pick(const Vec3& origin, const Vec3& direction, uint64_t& node, int& side, Vec3& point, Vec3& normal) const {
        apex_pick hit{};
        hit.struct_size = sizeof(hit);
        if (!live || !live->ray_pick || live->ray_pick(live->context, {toC(origin), toC(direction)}, &hit) != APEX_OK) return false;
        node = hit.node; side = hit.side; point = fromC(hit.point); normal = fromC(hit.normal);
        return true;
    }
    bool snap(const Vec3& origin, const Vec3& direction, Vec3& point) const {
        apex_vec3 out{};
        if (!live || !live->snap || live->snap(live->context, {toC(origin), toC(direction)}, &out) != APEX_OK) return false;
        point = fromC(out);
        return true;
    }
    bool project(const Vec3& p, double& px, double& py) const {
        return live && live->world_to_pixel && live->world_to_pixel(live->context, toC(p), &px, &py) == APEX_OK;
    }
};
std::vector<apex_plane> planesOf(const CScriptArray* array) {
    std::vector<apex_plane> out;
    if (!array) return out;
    out.reserve(array->GetSize());
    for (asUINT i = 0; i < array->GetSize(); ++i) {
        const auto* p = static_cast<const Plane*>(array->At(i));
        out.push_back({toC(p->normal), p->distance});
    }
    return out;
}
struct GhostData { std::vector<apex_plane> planes; string material; float opacity = 0.35f; };
struct Visual {
    int refs = 1;
    std::vector<apex_tool_segment> segments;
    std::vector<GhostData> ghosts;
    void addRef() { ++refs; }
    void release() { if (--refs == 0) delete this; }
    void clear() { segments.clear(); ghosts.clear(); }
    void line(const Vec3& a, const Vec3& b, float r, float g, float bl, float al, float width) {
        segments.push_back({sizeof(apex_tool_segment), width, toC(a), toC(b), {r, g, bl, al}});
    }
    void ghost(const CScriptArray* planes, const string& material, double opacity) {
        try { ghosts.push_back({planesOf(planes), material, static_cast<float>(opacity)}); }
        catch (...) { raise("out of memory in Visual::ghost"); }
    }
};
struct Param { string id, label; uint32_t kind = 0; double value = 0, minimum = 0, maximum = 0; std::vector<string> choices; };
struct Readout {
    int refs = 1;
    string detail, status;
    bool warn = false;
    std::vector<Param> params;
    void addRef() { ++refs; }
    void release() { if (--refs == 0) delete this; }
    void clear() { detail.clear(); status.clear(); warn = false; params.clear(); }
    void setDetail(const string& s) { detail = s; }
    void setStatus(const string& s) { status = s; }
    void setWarn(bool w) { warn = w; }
    void parameter(const string& id, const string& label, int kind, double value, double minimum, double maximum) {
        try { params.push_back({id, label, static_cast<uint32_t>(kind), value, minimum, maximum, {}}); }
        catch (...) { raise("out of memory in Readout::parameter"); }
    }
    void choice(const string& id, const string& label, const CScriptArray* choices, double value) {
        try {
            Param p{id, label, 0, value, 0, 0, {}};
            if (choices) for (asUINT i = 0; i < choices->GetSize(); ++i) p.choices.push_back(*static_cast<const string*>(choices->At(i)));
            p.maximum = p.choices.empty() ? 0 : static_cast<double>(p.choices.size() - 1);
            params.push_back(std::move(p));
        } catch (...) { raise("out of memory in Readout::choice"); }
    }
};
// An edit batch. The script records operations; commit() replays all of them inside ONE apex.edit apply,
// which the host runs as a single undo step and rolls back whole when any operation fails.
struct Edit {
    int refs = 1;
    struct Op {
        enum Kind { SetKey, Destroy, CreateSolid, Reparent, SetSideKey, RemoveDisplacement, Rebuild } kind;
        uint64_t node = 0, parent = 0;
        int32_t side = 0;
        size_t position = 0;
        string a, b;
        std::vector<apex_plane> planes;
    };
    std::vector<Op> ops;
    void addRef() { ++refs; }
    void release() { if (--refs == 0) delete this; }
    template <class F> void record(F&& fill) { try { Op op{}; fill(op); ops.push_back(std::move(op)); } catch (...) { raise("out of memory in Edit"); } }
    void setKey(uint64_t node, const string& key, const string& value) { record([&](Op& o) { o.kind = Op::SetKey; o.node = node; o.a = key; o.b = value; }); }
    void destroy(uint64_t node) { record([&](Op& o) { o.kind = Op::Destroy; o.node = node; }); }
    void createSolid(uint64_t parent, const CScriptArray* planes, const string& material) {
        record([&](Op& o) { o.kind = Op::CreateSolid; o.parent = parent; o.planes = planesOf(planes); o.a = material; });
    }
    void reparent(uint64_t node, uint64_t parent, asUINT position) { record([&](Op& o) { o.kind = Op::Reparent; o.node = node; o.parent = parent; o.position = position; }); }
    void setSideKey(int side, const string& key, const string& value) { record([&](Op& o) { o.kind = Op::SetSideKey; o.side = side; o.a = key; o.b = value; }); }
    void removeDisplacement(int side) { record([&](Op& o) { o.kind = Op::RemoveDisplacement; o.side = side; }); }
    void rebuildSolid(uint64_t node, const CScriptArray* planes) { record([&](Op& o) { o.kind = Op::Rebuild; o.node = node; o.planes = planesOf(planes); }); }
    size_t count() const { return ops.size(); }
    void discard() { ops.clear(); }
    bool commit(const string& label);
};
struct Replay { Edit* edit; };
apex_status replay(void* user, const apex_edit_ops_v1* ops, void* context) {
    for (const auto& op : static_cast<Replay*>(user)->edit->ops) {
        apex_status s = APEX_ERR_INVALID;
        switch (op.kind) {
            case Edit::Op::SetKey: s = ops->set_key(context, op.node, cstr(op.a), cstr(op.b)); break;
            case Edit::Op::Destroy: s = ops->destroy(context, op.node); break;
            case Edit::Op::CreateSolid: s = ops->create_solid(context, op.parent, op.planes.data(), op.planes.size(), cstr(op.a)); break;
            case Edit::Op::Reparent: s = ops->reparent(context, op.node, op.parent, op.position); break;
            case Edit::Op::SetSideKey: s = ops->set_side_key(context, op.side, cstr(op.a), cstr(op.b)); break;
            case Edit::Op::RemoveDisplacement: s = ops->remove_displacement(context, op.side); break;
            case Edit::Op::Rebuild: s = ops->rebuild_solid(context, op.node, op.planes.data(), op.planes.size()); break;
        }
        if (s != APEX_OK) return s;
    }
    return APEX_OK;
}
bool Edit::commit(const string& label) {
    auto* state = active();
    if (!state || !state->edit) { raise("apex.edit is not available"); return false; }
    if (ops.empty()) return true;   // nothing to apply: no undo step is created
    Replay r{this};
    const auto status = state->edit->apply(state->host->context, cstr(label), {}, &r, replay);
    ops.clear();
    if (status == APEX_OK) return true;
    const apex_str why = state->host->last_error ? state->host->last_error(state->host->context) : apex_str{};
    state->log("edit \"" + label + "\" was rolled back" + (why.size ? ": " + fromC(why) : string{}));
    return false;
}
Edit* editFactory() { return new Edit; }

// ---- tools --------------------------------------------------------------------------------------------------
struct ToolType {
    State* state = nullptr;
    asITypeInfo* type = nullptr;
    asIScriptFunction *activate = nullptr, *deactivate = nullptr, *input = nullptr, *visual = nullptr, *readout = nullptr,
                      *setParameter = nullptr, *perform = nullptr, *cancel = nullptr, *documentChanged = nullptr, *forget = nullptr;
};
// One per editor session, created by the host's create_instance. Owns the script object and the C++ views
// the script is handed. Everything pointed at from the apex_tool_* structs below lives until the next
// callback into this instance.
struct Instance {
    std::shared_ptr<State> keep;
    ToolType* type = nullptr;
    asIScriptObject* object = nullptr;
    Event* event = new Event;
    View* view = new View;
    Visual* visual = new Visual;
    Readout* readout = new Readout;
    std::vector<apex_tool_segment> segments;
    std::vector<apex_tool_ghost> ghosts;
    std::vector<apex_str> choiceViews;
    std::vector<apex_tool_parameter> parameters;
    string detail, status;
    ~Instance() {
        if (keep) keep->instances.erase(this);
        if (object) object->Release();
        event->release(); view->release(); visual->release(); readout->release();
    }
    State& s() const { return *keep; }
    template <class Bind, class Take> bool call(asIScriptFunction* f, std::chrono::milliseconds budget, Bind&& bind, Take&& take) {
        return object && f && run(*keep, f, object, budget, bind, take);
    }
    bool call(asIScriptFunction* f) { return call(f, kCallBudget, noBind, noTake); }
};
State::~State() { shutdown(); }

void State::shutdown() {
    if (!engine) return;
    for (auto* instance : instances) { if (instance->object) { instance->object->Release(); instance->object = nullptr; } }
    for (auto& action : actions) { if (action->function) { action->function->Release(); action->function = nullptr; } }
    if (onChanged) { onChanged->Release(); onChanged = nullptr; }
    engine->GarbageCollect();
    engine->ShutDownAndRelease();
    engine = nullptr; module = nullptr;
    --liveEngines;
}
Instance& self(void* p) { return *static_cast<Instance*>(p); }
void toolActivate(void* p) { try { auto& i = self(p); i.call(i.type->activate); } catch (...) {} }
void toolDeactivate(void* p) { try { auto& i = self(p); i.call(i.type->deactivate); } catch (...) {} }
int toolInput(void* p, const apex_tool_event* e, const apex_tool_view* v) try {
    auto& i = self(p);
    if (!e || !v || !i.type->input) return 0;
    auto& ev = *i.event;
    ev.kind = e->kind; ev.button = e->button; ev.modifiers = e->modifiers; ev.flags = e->flags;
    ev.source = e->source; ev.x = e->x; ev.y = e->y; ev.key = fromC(e->key);
    auto& vw = *i.view;
    vw.rayOrigin = fromC(v->ray.origin); vw.rayDirection = fromC(v->ray.direction);
    vw.cameraPosition = fromC(v->camera_position); vw.cameraAngles = fromC(v->camera_angles);
    vw.cameraForward = fromC(v->camera_forward); vw.cameraUp = fromC(v->camera_up);
    vw.width = v->width; vw.height = v->height; vw.fovY = v->fov_y_degrees; vw.orthoHeight = v->ortho_height;
    vw.orthographic = v->orthographic != 0;
    vw.live = v;
    bool handled = false;
    i.call(i.type->input, kCallBudget,
           [&](asIScriptContext* c) { c->SetArgObject(0, i.event); c->SetArgObject(1, i.view); },
           [&](asIScriptContext* c) { handled = c->GetReturnByte() != 0; });
    vw.live = nullptr;
    return handled;
} catch (...) { return 0; }
apex_status toolVisual(void* p, uint64_t source, apex_tool_visual* out) try {
    auto& i = self(p);
    if (!out) return APEX_ERR_INVALID;
    out->segments = nullptr; out->segment_count = 0; out->ghosts = nullptr; out->ghost_count = 0;
    if (!i.type->visual) return APEX_OK;
    i.visual->clear();
    const bool ok = i.call(i.type->visual, kCallBudget,
        [&](asIScriptContext* c) { c->SetArgQWord(0, source); c->SetArgObject(1, i.visual); }, noTake);
    if (!ok) return APEX_ERR_INTERNAL;
    i.segments = i.visual->segments;
    i.ghosts.clear();
    for (const auto& g : i.visual->ghosts) i.ghosts.push_back({sizeof(apex_tool_ghost), g.opacity, g.planes.data(), g.planes.size(), cstr(g.material)});
    out->segments = i.segments.data(); out->segment_count = i.segments.size();
    out->ghosts = i.ghosts.data(); out->ghost_count = i.ghosts.size();
    return APEX_OK;
} catch (...) { return APEX_ERR_INTERNAL; }
apex_status toolReadout(void* p, apex_tool_readout* out) try {
    auto& i = self(p);
    if (!out) return APEX_ERR_INVALID;
    if (!i.type->readout) return APEX_OK;
    i.readout->clear();
    if (!i.call(i.type->readout, kCallBudget, [&](asIScriptContext* c) { c->SetArgObject(0, i.readout); }, noTake)) return APEX_ERR_INTERNAL;
    const auto& r = *i.readout;
    i.detail = r.detail; i.status = r.status;
    i.choiceViews.clear();
    for (const auto& param : r.params) for (const auto& c : param.choices) i.choiceViews.push_back(cstr(c));
    i.parameters.clear();
    std::size_t at = 0;
    for (const auto& param : r.params) {
        i.parameters.push_back({sizeof(apex_tool_parameter), param.kind, cstr(param.id), cstr(param.label), param.value, param.minimum, param.maximum,
                                param.choices.empty() ? nullptr : i.choiceViews.data() + at, param.choices.size()});
        at += param.choices.size();
    }
    out->detail = cstr(i.detail); out->status = cstr(i.status); out->warn = r.warn ? 1u : 0u;
    out->parameters = i.parameters.data(); out->parameter_count = i.parameters.size();
    return APEX_OK;
} catch (...) { return APEX_ERR_INTERNAL; }
int toolSetParameter(void* p, apex_str id, double value) try {
    auto& i = self(p);
    string name = fromC(id);
    bool handled = false;
    i.call(i.type->setParameter, kCallBudget,
           [&](asIScriptContext* c) { c->SetArgObject(0, &name); c->SetArgDouble(1, value); },
           [&](asIScriptContext* c) { handled = c->GetReturnByte() != 0; });
    return handled;
} catch (...) { return 0; }
int toolPerform(void* p, apex_str action) try {
    auto& i = self(p);
    string name = fromC(action);
    bool handled = false;
    i.call(i.type->perform, kCallBudget, [&](asIScriptContext* c) { c->SetArgObject(0, &name); },
           [&](asIScriptContext* c) { handled = c->GetReturnByte() != 0; });
    return handled;
} catch (...) { return 0; }
void toolCancel(void* p) { try { auto& i = self(p); i.call(i.type->cancel); } catch (...) {} }
void toolDocumentChanged(void* p) { try { auto& i = self(p); i.call(i.type->documentChanged); } catch (...) {} }
void toolForget(void* p, uint64_t source) {
    try { auto& i = self(p); i.call(i.type->forget, kCallBudget, [&](asIScriptContext* c) { c->SetArgQWord(0, source); }, noTake); } catch (...) {}
}
void* toolCreate(void* registration) try {
    auto* type = static_cast<ToolType*>(registration);
    auto& state = *type->state;
    if (!state.engine) return nullptr;
    auto object = static_cast<asIScriptObject*>(state.engine->CreateScriptObject(type->type));
    if (!object) { state.fail(string("cannot create an instance of ") + type->type->GetName()); return nullptr; }
    auto instance = std::make_unique<Instance>();
    instance->keep = state.weak.lock();
    instance->type = type;
    instance->object = object;
    state.instances.insert(instance.get());
    return instance.release();
} catch (...) { return nullptr; }
void toolDestroy(void* p) { try { delete static_cast<Instance*>(p); } catch (...) {} }
const apex_tool_callbacks kToolCallbacks{
    sizeof(apex_tool_callbacks), 0, toolActivate, toolDeactivate, toolInput, toolVisual, toolReadout, toolSetParameter,
    toolPerform, toolCancel, toolDocumentChanged, toolForget, toolCreate, toolDestroy};

// ---- the apex:: namespace ---------------------------------------------------------------------------------
void scriptLog(const string& text) { if (auto* s = active()) s->log(text); }
bool registerTool(const string& name, const string& title, const string& description, const string& className) {
    auto* state = active();
    if (!state) return false;
    if (!state->loading) { raise("apex::registerTool is available only while main() runs"); return false; }
    const auto* api = static_cast<const apex_tools_v1*>(state->host->get_interface(state->host->context, apex::plugin::string("apex.tools/1")));
    if (!api) { raise("apex.tools is not available"); return false; }
    asITypeInfo* type = state->module->GetTypeInfoByName(className.c_str());
    if (!type || !(type->GetFlags() & asOBJ_SCRIPT_OBJECT)) { raise(("apex::registerTool: no script class named " + className).c_str()); return false; }
    bool defaultFactory = false;
    for (asUINT i = 0; i < type->GetFactoryCount(); ++i) if (type->GetFactoryByIndex(i)->GetParamCount() == 0) defaultFactory = true;
    if (!defaultFactory) { raise(("apex::registerTool: " + className + " needs a constructor without parameters").c_str()); return false; }
    auto entry = std::make_unique<ToolType>();
    entry->state = state;
    entry->type = type;
    struct Slot { const char* name; const char* declaration; asIScriptFunction* ToolType::*member; };
    static const Slot slots[] = {
        {"activate", "void activate()", &ToolType::activate},
        {"deactivate", "void deactivate()", &ToolType::deactivate},
        {"input", "bool input(apex::Event@, apex::View@)", &ToolType::input},
        {"visual", "void visual(uint64, apex::Visual@)", &ToolType::visual},
        {"readout", "void readout(apex::Readout@)", &ToolType::readout},
        {"setParameter", "bool setParameter(const string &in, double)", &ToolType::setParameter},
        {"perform", "bool perform(const string &in)", &ToolType::perform},
        {"cancel", "void cancel()", &ToolType::cancel},
        {"documentChanged", "void documentChanged()", &ToolType::documentChanged},
        {"forget", "void forget(uint64)", &ToolType::forget},
    };
    for (const auto& slot : slots) {
        auto* method = type->GetMethodByDecl(slot.declaration);
        if (!method && type->GetMethodByName(slot.name)) {
            raise((className + "::" + slot.name + " must be declared as '" + slot.declaration + "'").c_str());
            return false;
        }
        (*entry).*slot.member = method;
    }
    const string id = "plugin." + state->pluginId + "." + name;
    auto* raw = entry.get();
    if (api->register_tool(state->host->context, cstr(id), cstr(title), cstr(description), raw, &kToolCallbacks) != APEX_OK) {
        const auto why = state->host->last_error ? fromC(state->host->last_error(state->host->context)) : string{};
        raise(("apex::registerTool(" + name + ") was refused by the host: " + why).c_str());
        return false;
    }
    state->tools.push_back(std::move(entry));
    return true;
}
void actionInvoke(void* user) {
    auto* reg = static_cast<ActionReg*>(user);
    try { run(*reg->state, reg->function, nullptr, kCallBudget, noBind, noTake); } catch (...) {}
}
bool registerAction(const string& name, const string& title, const string& description, asIScriptFunction* function) {
    auto* state = active();
    if (!state || !function) return false;
    if (!state->loading) { raise("apex::registerAction is available only while main() runs"); return false; }
    const auto* api = static_cast<const apex_actions_v1*>(state->host->get_interface(state->host->context, apex::plugin::string("apex.actions/1")));
    if (!api) { raise("apex.actions is not available"); return false; }
    auto reg = std::make_unique<ActionReg>(ActionReg{state, function});
    const string id = "plugin." + state->pluginId + "." + name;
    if (api->register_action(state->host->context, cstr(id), cstr(title), cstr(description), reg.get(), actionInvoke, nullptr, 0) != APEX_OK) {
        const auto why = state->host->last_error ? fromC(state->host->last_error(state->host->context)) : string{};
        raise(("apex::registerAction(" + name + ") was refused by the host: " + why).c_str());
        return false;
    }
    function->AddRef();   // released by unload()
    state->actions.push_back(std::move(reg));
    return true;
}

// document
bool docOpen() {
    auto* s = active();
    apex_document_info info{}; info.struct_size = sizeof(info);
    return s && s->document && s->document->info(s->host->context, &info) == APEX_OK && info.is_open;
}
uint64_t docRevision() {
    auto* s = active();
    apex_document_info info{}; info.struct_size = sizeof(info);
    return s && s->document && s->document->info(s->host->context, &info) == APEX_OK ? info.revision : 0;
}
asUINT docRootCount() {
    auto* s = active();
    apex_document_info info{}; info.struct_size = sizeof(info);
    return s && s->document && s->document->info(s->host->context, &info) == APEX_OK ? static_cast<asUINT>(info.root_count) : 0;
}
uint64_t docRoot(asUINT index) {
    auto* s = active();
    apex_node_id id = 0;
    return s && s->document && s->document->root_at(s->host->context, index, &id) == APEX_OK ? id : 0;
}
bool nodeInfo(uint64_t id, apex_node_info& out, string* kind = nullptr) {
    auto* s = active();
    out = {}; out.struct_size = sizeof(out);
    if (!s || !s->document || s->document->node(s->host->context, id, &out) != APEX_OK) return false;
    if (kind) *kind = fromC(out.kind);
    return true;
}
string docKind(uint64_t id) { apex_node_info n; string k; return nodeInfo(id, n, &k) ? k : string{}; }
uint64_t docParent(uint64_t id) { apex_node_info n; return nodeInfo(id, n) ? n.parent : 0; }
asUINT docChildCount(uint64_t id) { apex_node_info n; return nodeInfo(id, n) ? static_cast<asUINT>(n.child_count) : 0; }
uint64_t docChild(uint64_t id, asUINT index) {
    auto* s = active();
    apex_node_id out = 0;
    return s && s->document && s->document->child_at(s->host->context, id, index, &out) == APEX_OK ? out : 0;
}
asUINT docKeyCount(uint64_t id) { apex_node_info n; return nodeInfo(id, n) ? static_cast<asUINT>(n.key_count) : 0; }
bool docKeyAt(uint64_t id, asUINT index, string& key, string& value) {
    auto* s = active();
    apex_str k{}, v{};
    if (!s || !s->document || s->document->key_at(s->host->context, id, index, &k, &v) != APEX_OK) return false;
    key = fromC(k); value = fromC(v);
    return true;
}
string docValue(uint64_t id, const string& key) {
    apex_node_info n;
    if (!nodeInfo(id, n)) return {};
    string k, v;
    for (size_t i = 0; i < n.key_count; ++i) if (docKeyAt(id, static_cast<asUINT>(i), k, v) && k == key) return v;
    return {};
}
CScriptArray* docKeys(uint64_t id) {
    auto* s = active();
    if (!s) return nullptr;
    auto* array = CScriptArray::Create(s->stringArray);
    apex_node_info n;
    if (!array || !nodeInfo(id, n)) return array;
    string k, v;
    for (size_t i = 0; i < n.key_count; ++i) if (docKeyAt(id, static_cast<asUINT>(i), k, v)) array->InsertLast(&k);
    return array;
}
// selection
asUINT selMode() {
    auto* s = active();
    uint32_t mode = 0; size_t count = 0;
    return s && s->selection && s->selection->info(s->host->context, &mode, &count) == APEX_OK ? mode : 0;
}
CScriptArray* selIds() {
    auto* s = active();
    if (!s) return nullptr;
    auto* array = CScriptArray::Create(s->idArray);
    uint32_t mode = 0; size_t count = 0;
    if (!array || !s->selection || s->selection->info(s->host->context, &mode, &count) != APEX_OK) return array;
    for (size_t i = 0; i < count; ++i) {
        uint64_t id = 0;
        if (s->selection->at(s->host->context, i, &id) == APEX_OK) array->InsertLast(&id);
    }
    return array;
}
bool selSet(asUINT mode, const CScriptArray* ids) {
    auto* s = active();
    if (!s || !s->selection) return false;
    std::vector<uint64_t> list;
    if (ids) for (asUINT i = 0; i < ids->GetSize(); ++i) list.push_back(*static_cast<const uint64_t*>(ids->At(i)));
    return s->selection->set(s->host->context, mode, list.data(), list.size()) == APEX_OK;
}

void onMessage(const asSMessageInfo* message, void* param) {
    auto& state = *static_cast<State*>(param);
    const char* kind = message->type == asMSGTYPE_ERROR ? "error" : message->type == asMSGTYPE_WARNING ? "warning" : "info";
    std::ostringstream line;
    line << (message->section && *message->section ? message->section : "script") << ':' << message->row << ':' << message->col
         << ": " << kind << ": " << message->message;
    if (!state.messages.empty()) state.messages += '\n';
    state.messages += line.str();
    if (message->type != asMSGTYPE_ERROR) state.log(line.str());
}
struct Reg {
    asIScriptEngine* engine;
    string failure;
    void operator()(int result, const char* what) { if (result < 0 && failure.empty()) failure = what; }
};
// Registers the whole script API. Returns the first failure, or an empty string.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"   // asMETHOD casts every member pointer, by design
#endif
string registerApi(asIScriptEngine* e) {
    Reg R{e, {}};
    RegisterScriptArray(e, true);
    RegisterStdString(e);
    RegisterScriptMath(e);
    R(e->RegisterGlobalFunction("void print(const string &in)", asFUNCTION(scriptLog), asCALL_CDECL), "print");
    R(e->SetDefaultNamespace("apex"), "namespace");
    R(e->RegisterGlobalFunction("void log(const string &in)", asFUNCTION(scriptLog), asCALL_CDECL), "log");

    const auto valueFlags = asOBJ_VALUE | asOBJ_POD | asOBJ_APP_CLASS_ALLFLOATS | asGetTypeTraits<Vec3>();
    R(e->RegisterObjectType("Vec3", sizeof(Vec3), valueFlags), "Vec3");
    R(e->RegisterObjectProperty("Vec3", "double x", asOFFSET(Vec3, x)), "Vec3.x");
    R(e->RegisterObjectProperty("Vec3", "double y", asOFFSET(Vec3, y)), "Vec3.y");
    R(e->RegisterObjectProperty("Vec3", "double z", asOFFSET(Vec3, z)), "Vec3.z");
    R(e->RegisterObjectBehaviour("Vec3", asBEHAVE_CONSTRUCT, "void f()", asFUNCTION(vecInit), asCALL_CDECL_OBJLAST), "Vec3()");
    R(e->RegisterObjectBehaviour("Vec3", asBEHAVE_CONSTRUCT, "void f(double, double, double)", asFUNCTION(vecInit3), asCALL_CDECL_OBJLAST), "Vec3(x,y,z)");
    R(e->RegisterObjectMethod("Vec3", "Vec3 opAdd(const Vec3 &in) const", asFUNCTION(vecAdd), asCALL_CDECL_OBJFIRST), "Vec3::opAdd");
    R(e->RegisterObjectMethod("Vec3", "Vec3 opSub(const Vec3 &in) const", asFUNCTION(vecSub), asCALL_CDECL_OBJFIRST), "Vec3::opSub");
    R(e->RegisterObjectMethod("Vec3", "Vec3 opMul(double) const", asFUNCTION(vecScale), asCALL_CDECL_OBJFIRST), "Vec3::opMul");
    R(e->RegisterObjectMethod("Vec3", "Vec3 opMul_r(double) const", asFUNCTION(vecScale), asCALL_CDECL_OBJFIRST), "Vec3::opMul_r");
    R(e->RegisterObjectMethod("Vec3", "Vec3 opNeg() const", asFUNCTION(vecNeg), asCALL_CDECL_OBJFIRST), "Vec3::opNeg");
    R(e->RegisterObjectMethod("Vec3", "double dot(const Vec3 &in) const", asFUNCTION(vecDot), asCALL_CDECL_OBJFIRST), "Vec3::dot");
    R(e->RegisterObjectMethod("Vec3", "Vec3 cross(const Vec3 &in) const", asFUNCTION(vecCross), asCALL_CDECL_OBJFIRST), "Vec3::cross");
    R(e->RegisterObjectMethod("Vec3", "double length() const", asFUNCTION(vecLength), asCALL_CDECL_OBJFIRST), "Vec3::length");
    R(e->RegisterObjectMethod("Vec3", "Vec3 normalized() const", asFUNCTION(vecNormalized), asCALL_CDECL_OBJFIRST), "Vec3::normalized");

    R(e->RegisterObjectType("Plane", sizeof(Plane), asOBJ_VALUE | asOBJ_POD | asOBJ_APP_CLASS_ALLFLOATS | asGetTypeTraits<Plane>()), "Plane");
    R(e->RegisterObjectProperty("Plane", "Vec3 normal", asOFFSET(Plane, normal)), "Plane.normal");
    R(e->RegisterObjectProperty("Plane", "double distance", asOFFSET(Plane, distance)), "Plane.distance");
    R(e->RegisterObjectBehaviour("Plane", asBEHAVE_CONSTRUCT, "void f()", asFUNCTION(planeInit), asCALL_CDECL_OBJLAST), "Plane()");
    R(e->RegisterObjectBehaviour("Plane", asBEHAVE_CONSTRUCT, "void f(const Vec3 &in, double)", asFUNCTION(planeInit2), asCALL_CDECL_OBJLAST), "Plane(n,d)");

    R(e->RegisterEnum("InputKind"), "InputKind");
    const char* kinds[] = {"Press", "Release", "Move", "KeyDown", "KeyUp", "Leave", "Cancel", "CancelPointer"};
    for (int i = 0; i < 8; ++i) R(e->RegisterEnumValue("InputKind", kinds[i], i), "InputKind value");
    R(e->RegisterEnum("Button"), "Button");
    const char* buttons[] = {"None", "Left", "Right", "Middle"};
    for (int i = 0; i < 4; ++i) R(e->RegisterEnumValue("Button", buttons[i], i), "Button value");
    R(e->RegisterEnum("ParamKind"), "ParamKind");
    R(e->RegisterEnumValue("ParamKind", "Choice", 0), "ParamKind::Choice");
    R(e->RegisterEnumValue("ParamKind", "Integer", 1), "ParamKind::Integer");
    R(e->RegisterEnumValue("ParamKind", "Number", 2), "ParamKind::Number");
    R(e->RegisterEnum("SelectionMode"), "SelectionMode");
    R(e->RegisterEnumValue("SelectionMode", "Objects", 0), "SelectionMode::Objects");
    R(e->RegisterEnumValue("SelectionMode", "Solids", 1), "SelectionMode::Solids");
    R(e->RegisterEnumValue("SelectionMode", "Faces", 2), "SelectionMode::Faces");

    const auto refType = [&](const char* name) {
        R(e->RegisterObjectType(name, 0, asOBJ_REF), name);
    };
    refType("Event"); refType("View"); refType("Visual"); refType("Readout"); refType("Edit");
    R(e->RegisterObjectBehaviour("Event", asBEHAVE_ADDREF, "void f()", asMETHOD(Event, addRef), asCALL_THISCALL), "Event addref");
    R(e->RegisterObjectBehaviour("Event", asBEHAVE_RELEASE, "void f()", asMETHOD(Event, release), asCALL_THISCALL), "Event release");
    R(e->RegisterObjectBehaviour("View", asBEHAVE_ADDREF, "void f()", asMETHOD(View, addRef), asCALL_THISCALL), "View addref");
    R(e->RegisterObjectBehaviour("View", asBEHAVE_RELEASE, "void f()", asMETHOD(View, release), asCALL_THISCALL), "View release");
    R(e->RegisterObjectBehaviour("Visual", asBEHAVE_ADDREF, "void f()", asMETHOD(Visual, addRef), asCALL_THISCALL), "Visual addref");
    R(e->RegisterObjectBehaviour("Visual", asBEHAVE_RELEASE, "void f()", asMETHOD(Visual, release), asCALL_THISCALL), "Visual release");
    R(e->RegisterObjectBehaviour("Readout", asBEHAVE_ADDREF, "void f()", asMETHOD(Readout, addRef), asCALL_THISCALL), "Readout addref");
    R(e->RegisterObjectBehaviour("Readout", asBEHAVE_RELEASE, "void f()", asMETHOD(Readout, release), asCALL_THISCALL), "Readout release");
    R(e->RegisterObjectBehaviour("Edit", asBEHAVE_ADDREF, "void f()", asMETHOD(Edit, addRef), asCALL_THISCALL), "Edit addref");
    R(e->RegisterObjectBehaviour("Edit", asBEHAVE_RELEASE, "void f()", asMETHOD(Edit, release), asCALL_THISCALL), "Edit release");
    R(e->RegisterObjectBehaviour("Edit", asBEHAVE_FACTORY, "Edit@ f()", asFUNCTION(editFactory), asCALL_CDECL), "Edit()");

    R(e->RegisterObjectProperty("Event", "const InputKind kind", asOFFSET(Event, kind)), "Event.kind");
    R(e->RegisterObjectProperty("Event", "const Button button", asOFFSET(Event, button)), "Event.button");
    R(e->RegisterObjectProperty("Event", "const uint modifiers", asOFFSET(Event, modifiers)), "Event.modifiers");
    R(e->RegisterObjectProperty("Event", "const uint flags", asOFFSET(Event, flags)), "Event.flags");
    R(e->RegisterObjectProperty("Event", "const uint64 source", asOFFSET(Event, source)), "Event.source");
    R(e->RegisterObjectProperty("Event", "const double x", asOFFSET(Event, x)), "Event.x");
    R(e->RegisterObjectProperty("Event", "const double y", asOFFSET(Event, y)), "Event.y");
    R(e->RegisterObjectProperty("Event", "const string key", asOFFSET(Event, key)), "Event.key");

    R(e->RegisterObjectProperty("View", "const Vec3 rayOrigin", asOFFSET(View, rayOrigin)), "View.rayOrigin");
    R(e->RegisterObjectProperty("View", "const Vec3 rayDirection", asOFFSET(View, rayDirection)), "View.rayDirection");
    R(e->RegisterObjectProperty("View", "const Vec3 cameraPosition", asOFFSET(View, cameraPosition)), "View.cameraPosition");
    R(e->RegisterObjectProperty("View", "const Vec3 cameraAngles", asOFFSET(View, cameraAngles)), "View.cameraAngles");
    R(e->RegisterObjectProperty("View", "const Vec3 cameraForward", asOFFSET(View, cameraForward)), "View.cameraForward");
    R(e->RegisterObjectProperty("View", "const Vec3 cameraUp", asOFFSET(View, cameraUp)), "View.cameraUp");
    R(e->RegisterObjectProperty("View", "const double width", asOFFSET(View, width)), "View.width");
    R(e->RegisterObjectProperty("View", "const double height", asOFFSET(View, height)), "View.height");
    R(e->RegisterObjectProperty("View", "const double fovY", asOFFSET(View, fovY)), "View.fovY");
    R(e->RegisterObjectProperty("View", "const double orthoHeight", asOFFSET(View, orthoHeight)), "View.orthoHeight");
    R(e->RegisterObjectProperty("View", "const bool orthographic", asOFFSET(View, orthographic)), "View.orthographic");
    R(e->RegisterObjectMethod("View", "bool pick(const Vec3 &in, const Vec3 &in, uint64 &out, int &out, Vec3 &out, Vec3 &out) const", asMETHOD(View, pick), asCALL_THISCALL), "View::pick");
    R(e->RegisterObjectMethod("View", "bool snap(const Vec3 &in, const Vec3 &in, Vec3 &out) const", asMETHOD(View, snap), asCALL_THISCALL), "View::snap");
    R(e->RegisterObjectMethod("View", "bool project(const Vec3 &in, double &out, double &out) const", asMETHOD(View, project), asCALL_THISCALL), "View::project");

    R(e->RegisterObjectMethod("Visual", "void line(const Vec3 &in, const Vec3 &in, float, float, float, float, float)", asMETHOD(Visual, line), asCALL_THISCALL), "Visual::line");
    R(e->RegisterObjectMethod("Visual", "void ghost(const array<Plane> &in, const string &in, double)", asMETHOD(Visual, ghost), asCALL_THISCALL), "Visual::ghost");

    R(e->RegisterObjectMethod("Readout", "void detail(const string &in)", asMETHOD(Readout, setDetail), asCALL_THISCALL), "Readout::detail");
    R(e->RegisterObjectMethod("Readout", "void status(const string &in)", asMETHOD(Readout, setStatus), asCALL_THISCALL), "Readout::status");
    R(e->RegisterObjectMethod("Readout", "void warn(bool)", asMETHOD(Readout, setWarn), asCALL_THISCALL), "Readout::warn");
    R(e->RegisterObjectMethod("Readout", "void parameter(const string &in, const string &in, ParamKind, double, double, double)", asMETHOD(Readout, parameter), asCALL_THISCALL), "Readout::parameter");
    R(e->RegisterObjectMethod("Readout", "void choice(const string &in, const string &in, const array<string> &in, double)", asMETHOD(Readout, choice), asCALL_THISCALL), "Readout::choice");

    R(e->RegisterObjectMethod("Edit", "void setKey(uint64, const string &in, const string &in)", asMETHOD(Edit, setKey), asCALL_THISCALL), "Edit::setKey");
    R(e->RegisterObjectMethod("Edit", "void destroy(uint64)", asMETHOD(Edit, destroy), asCALL_THISCALL), "Edit::destroy");
    R(e->RegisterObjectMethod("Edit", "void createSolid(uint64, const array<Plane> &in, const string &in)", asMETHOD(Edit, createSolid), asCALL_THISCALL), "Edit::createSolid");
    R(e->RegisterObjectMethod("Edit", "void reparent(uint64, uint64, uint)", asMETHOD(Edit, reparent), asCALL_THISCALL), "Edit::reparent");
    R(e->RegisterObjectMethod("Edit", "void setSideKey(int, const string &in, const string &in)", asMETHOD(Edit, setSideKey), asCALL_THISCALL), "Edit::setSideKey");
    R(e->RegisterObjectMethod("Edit", "void removeDisplacement(int)", asMETHOD(Edit, removeDisplacement), asCALL_THISCALL), "Edit::removeDisplacement");
    R(e->RegisterObjectMethod("Edit", "void rebuildSolid(uint64, const array<Plane> &in)", asMETHOD(Edit, rebuildSolid), asCALL_THISCALL), "Edit::rebuildSolid");
    R(e->RegisterObjectMethod("Edit", "uint count() const", asMETHOD(Edit, count), asCALL_THISCALL), "Edit::count");
    R(e->RegisterObjectMethod("Edit", "void discard()", asMETHOD(Edit, discard), asCALL_THISCALL), "Edit::discard");
    R(e->RegisterObjectMethod("Edit", "bool commit(const string &in)", asMETHOD(Edit, commit), asCALL_THISCALL), "Edit::commit");

    R(e->RegisterFuncdef("void ActionFn()"), "ActionFn");
    R(e->RegisterGlobalFunction("bool registerTool(const string &in, const string &in, const string &in, const string &in)", asFUNCTION(registerTool), asCALL_CDECL), "registerTool");
    R(e->RegisterGlobalFunction("bool registerAction(const string &in, const string &in, const string &in, ActionFn@)", asFUNCTION(registerAction), asCALL_CDECL), "registerAction");

    R(e->RegisterGlobalFunction("bool documentOpen()", asFUNCTION(docOpen), asCALL_CDECL), "documentOpen");
    R(e->RegisterGlobalFunction("uint64 revision()", asFUNCTION(docRevision), asCALL_CDECL), "revision");
    R(e->RegisterGlobalFunction("uint rootCount()", asFUNCTION(docRootCount), asCALL_CDECL), "rootCount");
    R(e->RegisterGlobalFunction("uint64 root(uint)", asFUNCTION(docRoot), asCALL_CDECL), "root");
    R(e->RegisterGlobalFunction("string kind(uint64)", asFUNCTION(docKind), asCALL_CDECL), "kind");
    R(e->RegisterGlobalFunction("uint64 parent(uint64)", asFUNCTION(docParent), asCALL_CDECL), "parent");
    R(e->RegisterGlobalFunction("uint childCount(uint64)", asFUNCTION(docChildCount), asCALL_CDECL), "childCount");
    R(e->RegisterGlobalFunction("uint64 child(uint64, uint)", asFUNCTION(docChild), asCALL_CDECL), "child");
    R(e->RegisterGlobalFunction("uint keyCount(uint64)", asFUNCTION(docKeyCount), asCALL_CDECL), "keyCount");
    R(e->RegisterGlobalFunction("array<string>@ keys(uint64)", asFUNCTION(docKeys), asCALL_CDECL), "keys");
    R(e->RegisterGlobalFunction("string value(uint64, const string &in)", asFUNCTION(docValue), asCALL_CDECL), "value");
    R(e->RegisterGlobalFunction("SelectionMode selectionMode()", asFUNCTION(selMode), asCALL_CDECL), "selectionMode");
    R(e->RegisterGlobalFunction("array<uint64>@ selection()", asFUNCTION(selIds), asCALL_CDECL), "selection");
    R(e->RegisterGlobalFunction("bool setSelection(SelectionMode, const array<uint64> &in)", asFUNCTION(selSet), asCALL_CDECL), "setSelection");
    R(e->SetDefaultNamespace(""), "namespace reset");
    return R.failure;
}
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

class AsEngine final : public ScriptEngine {
public:
    ~AsEngine() override { unload(); }
    bool load(const ScriptSource& source, const apex_host* host, std::string& error) override {
        unload();
        std::ifstream in(source.file, std::ios::binary);
        if (!in) { error = "cannot read " + source.file.string(); return false; }
        const string code((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        auto state = std::make_shared<State>();
        state->weak = state;
        state->host = host;
        state->pluginId = source.pluginId;
        state->section = source.file.filename().string();
        state->engine = asCreateScriptEngine();
        if (!state->engine) { error = "cannot create the AngelScript engine"; return false; }
        ++liveEngines;
        auto* e = state->engine;
        e->SetUserData(state.get(), kUserData);
        e->SetEngineProperty(asEP_MAX_CALL_STACK_SIZE, 256);
        e->SetEngineProperty(asEP_ALLOW_MULTILINE_STRINGS, true);
        e->SetMessageCallback(asFUNCTION(onMessage), state.get(), asCALL_CDECL);
        if (auto failure = registerApi(e); !failure.empty()) { error = "AngelScript API registration failed at " + failure; state->shutdown(); return false; }
        state->stringArray = e->GetTypeInfoByDecl("array<string>");
        state->idArray = e->GetTypeInfoByDecl("array<uint64>");
        const auto interfaceOf = [&](const char* name) { return host->get_interface(host->context, apex::plugin::string(name)); };
        state->document = static_cast<const apex_document_v1*>(interfaceOf("apex.document/1"));
        state->edit = static_cast<const apex_edit_v1*>(interfaceOf("apex.edit/1"));
        state->selection = static_cast<const apex_selection_v1*>(interfaceOf("apex.selection/1"));
        state->module = e->GetModule("script", asGM_ALWAYS_CREATE);
        if (state->module->AddScriptSection(state->section.c_str(), code.data(), code.size()) < 0 || state->module->Build() < 0) {
            error = state->messages.empty() ? "the script did not compile" : state->messages;
            state->shutdown();
            return false;
        }
        auto* entry = state->module->GetFunctionByDecl("void main()");
        if (!entry) { error = state->section + ": the script has no 'void main()' entry point"; state->shutdown(); return false; }
        state->onChanged = state->module->GetFunctionByDecl("void onDocumentChanged(uint64)");
        if (state->onChanged) state->onChanged->AddRef();
        state->loading = true;
        const bool ok = run(*state, entry, nullptr, kLoadBudget, noBind, noTake);
        state->loading = false;
        if (!ok) {
            error = state->lastError.empty() ? "main() failed" : state->lastError;
            state->shutdown();
            return false;
        }
        state_ = std::move(state);
        return true;
    }
    void unload() override {
        if (!state_) return;
        state_->shutdown();
        state_.reset();
    }
    void documentChanged(const apex_change_notice& notice) override {
        if (!state_ || !state_->onChanged) return;
        run(*state_, state_->onChanged, nullptr, kCallBudget,
            [&](asIScriptContext* c) { c->SetArgQWord(0, notice.revision); }, noTake);
    }
private:
    std::shared_ptr<State> state_;
};
}  // namespace
#endif

ScriptEngineFactory defaultScriptEngineFactory() {
#if defined(APEX_HAVE_ANGELSCRIPT)
    return [](std::string_view runtime) -> std::unique_ptr<ScriptEngine> {
        if (runtime == "angelscript") return std::make_unique<AsEngine>();
        return nullptr;
    };
#else
    return {};
#endif
}
std::size_t liveAngelScriptEngines() {
#if defined(APEX_HAVE_ANGELSCRIPT)
    return liveEngines.load();
#else
    return 0;
#endif
}
}

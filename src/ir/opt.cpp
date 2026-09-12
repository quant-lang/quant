#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "quant/ir/opt.h"

namespace quant::codegen {

OptStats::Pass& OptStats::pass(const std::string& name) {
    for (auto& p : passes) {
        if (p.name == name) return p;
    }
    passes.push_back(Pass{name});
    return passes.back();
}

namespace {

using ast::TypeKind;

constexpr uint32_t kNone = std::numeric_limits<uint32_t>::max();

// Every pass runs once per round; rounds repeat until nothing changes.
constexpr int kMaxRounds = 8;

// Interpreter step budget for compile-time loop evaluation, per loop.
constexpr int64_t kLoopBudgetO2 = 16'000'000;
constexpr int64_t kLoopBudgetO3 = 128'000'000;

bool skip_pass(const char* name) {
    static const std::string skip = [] {
        const char* env = std::getenv("QUANT_SKIP");
        return std::string(env ? env : "");
    }();
    return !skip.empty() && skip.find(name) != std::string::npos;
}

template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};

template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

// Type helpers

bool is_signed_int_kind(TypeKind kind) {
    return kind >= TypeKind::I8 && kind <= TypeKind::I64;
}

bool is_unsigned_int_kind(TypeKind kind) {
    return kind >= TypeKind::U8 && kind <= TypeKind::U64;
}

bool is_int_kind(TypeKind kind) {
    return is_signed_int_kind(kind) || is_unsigned_int_kind(kind) || kind == TypeKind::Bool;
}

bool is_float_kind(TypeKind kind) {
    return kind == TypeKind::F32 || kind == TypeKind::F64;
}

int int_kind_size(TypeKind kind) {
    switch (kind) {
        case TypeKind::I8:  case TypeKind::U8:  return 1;
        case TypeKind::I16: case TypeKind::U16: return 2;
        case TypeKind::I32: case TypeKind::U32: return 4;
        case TypeKind::I64: case TypeKind::U64: return 8;
        default: return 0;
    }
}

bool op_is_compare(IRBinaryOp op) {
    switch (op) {
        case IRBinaryOp::Eq: case IRBinaryOp::NotEq:
        case IRBinaryOp::Lt: case IRBinaryOp::Lte:
        case IRBinaryOp::Gt: case IRBinaryOp::Gte:
            return true;
        default:
            return false;
    }
}

// Instruction shape: every instruction defines at most one temp (`dst`),
// uses a handful of temps, and either has an observable effect or not

template <class T>
constexpr bool has_dst_v = requires(T t) { t.dst; };

std::optional<Reg> def_of(const IRInst& inst) {
    return std::visit([](const auto& x) -> std::optional<Reg> {
        (void)x;
        if constexpr (has_dst_v<std::decay_t<decltype(x)>>) return x.dst;
        else return std::nullopt;
    }, inst);
}

// Instructions that must survive even when their result is unused
bool has_side_effect(const IRInst& inst) {
    return std::visit([](const auto& x) {
        using T = std::decay_t<decltype(x)>;
        (void)x;
        if constexpr (std::is_same_v<T, IRCall> || std::is_same_v<T, IRRegionAlloc>) return true;
        else return !has_dst_v<T>;
    }, inst);
}

bool is_terminator(const IRInst& inst) {
    return std::holds_alternative<IRJump>(inst) ||
           std::holds_alternative<IRBranch>(inst) ||
           std::holds_alternative<IRReturn>(inst);
}

template <class Inst, class F>
void for_each_use(Inst& inst, F&& f) {
    std::visit([&](auto& x) {
        using T = std::decay_t<decltype(x)>;
        (void)x;
        if constexpr (std::is_same_v<T, IRBinary>)            { f(x.lhs); f(x.rhs); }
        else if constexpr (std::is_same_v<T, IRCall>)         { for (auto& a : x.args) f(a); }
        else if constexpr (std::is_same_v<T, IRReturn>)       { f(x.value); }
        else if constexpr (std::is_same_v<T, IRBranch>)       { f(x.cond); }
        else if constexpr (std::is_same_v<T, IRGetField>)     { f(x.base); }
        else if constexpr (std::is_same_v<T, IRSetField>)     { f(x.base); f(x.value); }
        else if constexpr (std::is_same_v<T, IRCast>)         { f(x.src); }
        else if constexpr (std::is_same_v<T, IRLoadElement>)  { f(x.base); f(x.index); }
        else if constexpr (std::is_same_v<T, IRStoreElement>) { f(x.base); f(x.index); f(x.value); }
        else if constexpr (std::is_same_v<T, IRStoreLocal>)   { f(x.src); }
        else if constexpr (std::is_same_v<T, IRStoreGlobal>)  { f(x.src); }
        else if constexpr (std::is_same_v<T, IRRegionAlloc>)  { f(x.size); }
    }, inst);
}

// Locals read by an instruction (the slot itself, not memory behind it)
template <class F>
void for_each_local_use(const IRInst& inst, F&& f) {
    std::visit([&](const auto& x) {
        using T = std::decay_t<decltype(x)>;
        (void)x;
        if constexpr (std::is_same_v<T, IRLoadLocal>)         { f(x.local); }
        else if constexpr (std::is_same_v<T, IRAddrOf>)       { f(x.local); }
        else if constexpr (std::is_same_v<T, IRRegionBegin>)  { f(x.region_local); }
        else if constexpr (std::is_same_v<T, IRRegionAlloc>)  { f(x.region_local); }
        else if constexpr (std::is_same_v<T, IRRegionEnd>)    { f(x.region_local); }
    }, inst);
}

// Temps are numbered from zero; the count is authoritative, but tolerate a
// body that references more (defensive against a stale temp_count)
size_t reg_capacity(const IRFunction& fn) {
    size_t cap = fn.temp_count;
    for (const auto& inst : fn.body) {
        if (const auto d = def_of(inst)) cap = std::max<size_t>(cap, *d + 1u);
    }
    return cap;
}

void erase_marked(std::vector<IRInst>& body, const std::vector<uint8_t>& dead) {
    size_t write = 0;
    for (size_t read = 0; read < body.size(); ++read) {
        if (dead[read]) continue;
        if (write != read) body[write] = std::move(body[read]);
        ++write;
    }
    body.resize(write);
}

bool any_marked(const std::vector<uint8_t>& marks) {
    return std::any_of(marks.begin(), marks.end(), [](uint8_t m) { return m != 0; });
}

// Deferred temp renaming. Temps are single-assignment, so replacing every use
// of `from` by `to` is valid as long as `to` is defined before `from` — which
// holds for every rename the passes below produce (a value is only ever
// replaced by one of its own operands or by a value stored earlier).
class RegRenamer {
public:
    void add(Reg from, Reg to) {
        if (from != to) map_[from] = to;
    }

    bool empty() const { return map_.empty(); }

    Reg resolve(Reg reg) const {
        for (int hop = 0; hop < 64; ++hop) {
            const auto it = map_.find(reg);
            if (it == map_.end()) break;
            reg = it->second;
        }
        return reg;
    }

    void apply(std::vector<IRInst>& body) const {
        if (map_.empty()) return;
        for (auto& inst : body) {
            for_each_use(inst, [&](Reg& reg) { reg = resolve(reg); });
        }
    }

private:
    std::unordered_map<Reg, Reg> map_;
};

// Constant temps of a function. Temps are single-assignment, so one scan of
// the body gives a complete map that stays valid while instructions are
// replaced in place.
struct RegConsts {
    enum Tag : uint8_t { None, Int, Float };

    std::vector<Tag> tag;
    std::vector<int64_t> ival;
    std::vector<double> fval;
    std::vector<TypeKind> fkind;

    explicit RegConsts(size_t capacity)
        : tag(capacity, None), ival(capacity), fval(capacity), fkind(capacity, TypeKind::F64) {}

    void scan(const std::vector<IRInst>& body) {
        for (const auto& inst : body) {
            if (const auto* c = std::get_if<IRLoadConst>(&inst)) {
                set_int(c->dst, c->value);
            } else if (const auto* f = std::get_if<IRLoadFloatConst>(&inst)) {
                set_float(f->dst, f->value, f->kind);
            }
        }
    }

    void set_int(Reg reg, int64_t value) {
        if (reg >= tag.size()) return;
        tag[reg] = Int;
        ival[reg] = value;
    }

    void set_float(Reg reg, double value, TypeKind kind) {
        if (reg >= tag.size()) return;
        tag[reg] = Float;
        fval[reg] = value;
        fkind[reg] = kind;
    }

    bool get_int(Reg reg, int64_t& out) const {
        if (reg >= tag.size() || tag[reg] != Int) return false;
        out = ival[reg];
        return true;
    }

    bool get_float(Reg reg, double& out, TypeKind& kind) const {
        if (reg >= tag.size() || tag[reg] != Float) return false;
        out = fval[reg];
        kind = fkind[reg];
        return true;
    }
};

// Constant evaluation. Integer arithmetic mirrors the backends: every value
// lives in a 64-bit slot and wraps at 64 bits regardless of the declared
// width; division by zero and INT64_MIN / -1 are left to the runtime.

std::optional<int64_t> fold_int(IRBinaryOp op, int64_t lhs, int64_t rhs, TypeKind kind) {
    const uint64_t a = static_cast<uint64_t>(lhs);
    const uint64_t b = static_cast<uint64_t>(rhs);

    // Equality is a plain 64-bit compare for pointers and strings as well.
    if (is_float_kind(kind)) return std::nullopt;
    if (op == IRBinaryOp::Eq) return a == b;
    if (op == IRBinaryOp::NotEq) return a != b;
    if (!is_int_kind(kind)) return std::nullopt;

    const bool is_signed = is_signed_int_kind(kind);
    switch (op) {
        case IRBinaryOp::Add: return static_cast<int64_t>(a + b);
        case IRBinaryOp::Sub: return static_cast<int64_t>(a - b);
        case IRBinaryOp::Mul: return static_cast<int64_t>(a * b);
        case IRBinaryOp::Div:
            if (b == 0) return std::nullopt;
            if (is_signed) {
                if (lhs == std::numeric_limits<int64_t>::min() && rhs == -1) return std::nullopt;
                return lhs / rhs;
            }
            return static_cast<int64_t>(a / b);
        case IRBinaryOp::Lt:  return is_signed ? lhs < rhs  : a < b;
        case IRBinaryOp::Lte: return is_signed ? lhs <= rhs : a <= b;
        case IRBinaryOp::Gt:  return is_signed ? lhs > rhs  : a > b;
        case IRBinaryOp::Gte: return is_signed ? lhs >= rhs : a >= b;
        case IRBinaryOp::BitAnd:   return static_cast<int64_t>(a & b);
        case IRBinaryOp::BitOr:    return static_cast<int64_t>(a | b);
        case IRBinaryOp::LogicAnd: return a != 0 && b != 0;
        case IRBinaryOp::LogicOr:  return a != 0 || b != 0;
        default: return std::nullopt;
    }
}

std::optional<double> fold_float_arith(IRBinaryOp op, double lhs, double rhs) {
    switch (op) {
        case IRBinaryOp::Add: return lhs + rhs;
        case IRBinaryOp::Sub: return lhs - rhs;
        case IRBinaryOp::Mul: return lhs * rhs;
        case IRBinaryOp::Div: return rhs == 0.0 ? std::nullopt : std::optional<double>(lhs / rhs);
        default: return std::nullopt;
    }
}

std::optional<int64_t> fold_float_compare(IRBinaryOp op, double lhs, double rhs) {
    switch (op) {
        case IRBinaryOp::Eq:    return lhs == rhs;
        case IRBinaryOp::NotEq: return lhs != rhs;
        case IRBinaryOp::Lt:    return lhs < rhs;
        case IRBinaryOp::Lte:   return lhs <= rhs;
        case IRBinaryOp::Gt:    return lhs > rhs;
        case IRBinaryOp::Gte:   return lhs >= rhs;
        default: return std::nullopt;
    }
}

// Integer casts that every backend lowers identically: bit reinterpretation
// (an 8-byte copy), same-width value casts, and widening value casts, which
// extend the low bytes of the slot by the source signedness. Narrowing casts
// are not folded: x86-64 sign-extends to a signed target while AArch64
// zero-extends, so there is no single value to fold to.
std::optional<int64_t> fold_cast(const IRCast& cast, int64_t value) {
    if (cast.kind == ast::CastKind::Bitcast) return value;

    const int src = int_kind_size(cast.src_kind);
    const int dst = int_kind_size(cast.target_kind);
    if (src == 0 || dst == 0 || src > dst) return std::nullopt;
    if (src == dst) return value;

    const uint64_t bits = static_cast<uint64_t>(value);
    switch (src) {
        case 1: return is_signed_int_kind(cast.src_kind)
                    ? static_cast<int64_t>(static_cast<int8_t>(bits))
                    : static_cast<int64_t>(static_cast<uint8_t>(bits));
        case 2: return is_signed_int_kind(cast.src_kind)
                    ? static_cast<int64_t>(static_cast<int16_t>(bits))
                    : static_cast<int64_t>(static_cast<uint16_t>(bits));
        case 4: return is_signed_int_kind(cast.src_kind)
                    ? static_cast<int64_t>(static_cast<int32_t>(bits))
                    : static_cast<int64_t>(static_cast<uint32_t>(bits));
        default: return std::nullopt;
    }
}

// Control-flow graph over the flat instruction list. A block starts at index
// 0, at every label and after every terminator, so unreachable tails after a
// jump form their own blocks instead of hiding inside a live one.

using LabelIndex = std::unordered_map<Label, size_t>;

LabelIndex build_label_index(const std::vector<IRInst>& body) {
    LabelIndex index;
    for (size_t i = 0; i < body.size(); ++i) {
        if (const auto* l = std::get_if<IRLabel>(&body[i])) index[l->id] = i;
    }
    return index;
}

struct Cfg {
    size_t inst_count = 0;
    std::vector<uint32_t> leaders;
    std::vector<uint32_t> block_of;
    std::unordered_map<Label, uint32_t> label_block;
    std::vector<std::vector<uint32_t>> succ;
    std::vector<std::vector<uint32_t>> pred;

    uint32_t count() const { return static_cast<uint32_t>(leaders.size()); }
    uint32_t begin(uint32_t block) const { return leaders[block]; }
    uint32_t end(uint32_t block) const {
        return block + 1 < count() ? leaders[block + 1] : static_cast<uint32_t>(inst_count);
    }
};

Cfg build_cfg(const std::vector<IRInst>& body) {
    Cfg cfg;
    const size_t n = body.size();
    cfg.inst_count = n;
    if (n == 0) return cfg;

    cfg.leaders.push_back(0);
    for (size_t i = 0; i < n; ++i) {
        if (std::holds_alternative<IRLabel>(body[i]) && cfg.leaders.back() != i) {
            cfg.leaders.push_back(static_cast<uint32_t>(i));
        }
        if (is_terminator(body[i]) && i + 1 < n && cfg.leaders.back() != i + 1) {
            cfg.leaders.push_back(static_cast<uint32_t>(i + 1));
        }
    }

    cfg.block_of.resize(n);
    for (uint32_t b = 0; b < cfg.count(); ++b) {
        for (uint32_t i = cfg.begin(b); i < cfg.end(b); ++i) cfg.block_of[i] = b;
    }
    for (size_t i = 0; i < n; ++i) {
        if (const auto* l = std::get_if<IRLabel>(&body[i])) cfg.label_block[l->id] = cfg.block_of[i];
    }

    cfg.succ.resize(cfg.count());
    cfg.pred.resize(cfg.count());
    const auto link = [&](uint32_t from, Label to) {
        const auto it = cfg.label_block.find(to);
        if (it == cfg.label_block.end()) return;
        auto& out = cfg.succ[from];
        if (std::find(out.begin(), out.end(), it->second) != out.end()) return;
        out.push_back(it->second);
        cfg.pred[it->second].push_back(from);
    };
    for (uint32_t b = 0; b < cfg.count(); ++b) {
        const IRInst& last = body[cfg.end(b) - 1];
        if (const auto* j = std::get_if<IRJump>(&last)) {
            link(b, j->target);
        } else if (const auto* br = std::get_if<IRBranch>(&last)) {
            link(b, br->then_label);
            link(b, br->else_label);
        } else if (!std::holds_alternative<IRReturn>(last) && b + 1 < cfg.count()) {
            cfg.succ[b].push_back(b + 1);
            cfg.pred[b + 1].push_back(b);
        }
    }
    return cfg;
}

std::vector<uint8_t> reachable_blocks(const Cfg& cfg) {
    std::vector<uint8_t> reached(cfg.count());
    if (cfg.count() == 0) return reached;
    std::vector<uint32_t> work{0};
    reached[0] = 1;
    while (!work.empty()) {
        const uint32_t b = work.back();
        work.pop_back();
        for (uint32_t s : cfg.succ[b]) {
            if (!reached[s]) {
                reached[s] = 1;
                work.push_back(s);
            }
        }
    }
    return reached;
}

struct BitSet {
    std::vector<uint64_t> words;

    explicit BitSet(size_t bits = 0) : words((bits + 63) / 64) {}

    bool test(size_t i) const { return (words[i / 64] >> (i % 64)) & 1u; }
    void set(size_t i) { words[i / 64] |= uint64_t{1} << (i % 64); }
    void reset(size_t i) { words[i / 64] &= ~(uint64_t{1} << (i % 64)); }

    void merge(const BitSet& other) {
        for (size_t w = 0; w < words.size(); ++w) words[w] |= other.words[w];
    }

    bool operator==(const BitSet& other) const { return words == other.words; }
};

std::vector<uint8_t> address_taken_locals(const IRFunction& fn) {
    std::vector<uint8_t> escaped(fn.local_count);
    for (const auto& inst : fn.body) {
        if (const auto* a = std::get_if<IRAddrOf>(&inst)) {
            if (a->local < escaped.size()) escaped[a->local] = 1;
        }
    }
    return escaped;
}

// Pass: constant folding, algebraic simplification, cast and branch folding.

bool fold_constants(IRFunction& fn, bool fold_branches, OptStats* stats) {
    RegConsts consts(reg_capacity(fn));
    consts.scan(fn.body);

    RegRenamer rename;
    std::vector<uint8_t> dead(fn.body.size());
    bool changed = false;

    const auto make_int = [&](size_t i, Reg dst, int64_t value) {
        fn.body[i] = IRLoadConst{dst, value};
        consts.set_int(dst, value);
        changed = true;
    };
    const auto make_float = [&](size_t i, Reg dst, double value, TypeKind kind) {
        fn.body[i] = IRLoadFloatConst{dst, value, kind};
        consts.set_float(dst, value, kind);
        changed = true;
    };
    const auto forward = [&](size_t i, Reg dst, Reg keep) {
        rename.add(dst, keep);
        dead[i] = 1;
        changed = true;
    };

    for (size_t i = 0; i < fn.body.size(); ++i) {
        IRInst& inst = fn.body[i];

        if (auto* b = std::get_if<IRBinary>(&inst)) {
            b->lhs = rename.resolve(b->lhs);
            b->rhs = rename.resolve(b->rhs);
            const IRBinaryOp op = b->op;
            const TypeKind kind = b->type_kind;
            const Reg dst = b->dst;
            const Reg lhs = b->lhs;
            const Reg rhs = b->rhs;

            if (is_float_kind(kind)) {
                double l = 0, r = 0;
                TypeKind lk, rk;
                if (!consts.get_float(lhs, l, lk) || !consts.get_float(rhs, r, rk)) continue;
                // f32 operands are rounded to single precision at runtime.
                if (kind == TypeKind::F32) {
                    l = static_cast<double>(static_cast<float>(l));
                    r = static_cast<double>(static_cast<float>(r));
                }
                if (op_is_compare(op)) {
                    if (const auto v = fold_float_compare(op, l, r)) make_int(i, dst, *v);
                } else if (auto v = fold_float_arith(op, l, r)) {
                    if (kind == TypeKind::F32) v = static_cast<double>(static_cast<float>(*v));
                    make_float(i, dst, *v, kind);
                }
                continue;
            }

            int64_t l = 0, r = 0;
            const bool has_l = consts.get_int(lhs, l);
            const bool has_r = consts.get_int(rhs, r);

            if (has_l && has_r) {
                if (const auto v = fold_int(op, l, r, kind)) {
                    make_int(i, dst, *v);
                    continue;
                }
            }

            if (lhs == rhs) {
                if (op == IRBinaryOp::Eq)    { make_int(i, dst, 1); continue; }
                if (op == IRBinaryOp::NotEq) { make_int(i, dst, 0); continue; }
            }
            if (!is_int_kind(kind)) continue;

            if (lhs == rhs) {
                switch (op) {
                    case IRBinaryOp::Sub: make_int(i, dst, 0); continue;
                    case IRBinaryOp::Lt: case IRBinaryOp::Gt: make_int(i, dst, 0); continue;
                    case IRBinaryOp::Lte: case IRBinaryOp::Gte: make_int(i, dst, 1); continue;
                    case IRBinaryOp::BitAnd: case IRBinaryOp::BitOr: forward(i, dst, lhs); continue;
                    default: break;
                }
            }

            const bool l_zero = has_l && l == 0;
            const bool r_zero = has_r && r == 0;
            const bool l_one = has_l && l == 1;
            const bool r_one = has_r && r == 1;
            const bool l_true = has_l && l != 0;
            const bool r_true = has_r && r != 0;

            switch (op) {
                case IRBinaryOp::Add:
                    if (r_zero) forward(i, dst, lhs);
                    else if (l_zero) forward(i, dst, rhs);
                    break;
                case IRBinaryOp::Sub:
                    if (r_zero) forward(i, dst, lhs);
                    break;
                case IRBinaryOp::Mul:
                    if (l_zero || r_zero) make_int(i, dst, 0);
                    else if (r_one) forward(i, dst, lhs);
                    else if (l_one) forward(i, dst, rhs);
                    break;
                case IRBinaryOp::Div:
                    if (r_one) forward(i, dst, lhs);
                    break;
                case IRBinaryOp::BitAnd:
                    if (l_zero || r_zero) make_int(i, dst, 0);
                    break;
                case IRBinaryOp::BitOr:
                    if (r_zero) forward(i, dst, lhs);
                    else if (l_zero) forward(i, dst, rhs);
                    break;
                case IRBinaryOp::LogicAnd:
                    if (l_zero || r_zero) make_int(i, dst, 0);
                    break;
                case IRBinaryOp::LogicOr:
                    if (l_true || r_true) make_int(i, dst, 1);
                    break;
                default:
                    break;
            }
        } else if (auto* c = std::get_if<IRCast>(&inst)) {
            c->src = rename.resolve(c->src);
            int64_t v = 0;
            if (!consts.get_int(c->src, v)) continue;
            if (const auto folded = fold_cast(*c, v)) make_int(i, c->dst, *folded);
        } else if (auto* br = std::get_if<IRBranch>(&inst)) {
            if (!fold_branches) continue;
            br->cond = rename.resolve(br->cond);
            int64_t v = 0;
            if (br->then_label == br->else_label) {
                inst = IRJump{br->then_label};
                changed = true;
            } else if (consts.get_int(br->cond, v)) {
                inst = IRJump{v != 0 ? br->then_label : br->else_label};
                changed = true;
                if (stats) ++stats->branches_folded;
            }
        }
    }

    rename.apply(fn.body);
    if (any_marked(dead)) erase_marked(fn.body, dead);
    return changed;
}

// Local slot analysis: forward dataflow computing, at the start of every
// block, which locals hold a known constant or a known temp. Locals whose
// address is taken are never tracked. Nothing but IRStoreLocal writes a slot
// (calls cannot reach a slot whose address was never taken), so the transfer
// function only has to look at stores.

struct LocalValue {
    enum Tag : uint8_t { Unknown, Int, Float, Temp } tag = Unknown;
    int64_t ival = 0;
    double fval = 0;
    TypeKind fkind = TypeKind::F64;
    Reg reg = 0;

    bool operator==(const LocalValue& o) const {
        if (tag != o.tag) return false;
        switch (tag) {
            case Int:   return ival == o.ival;
            case Float: return fval == o.fval && fkind == o.fkind;
            case Temp:  return reg == o.reg;
            default:    return true;
        }
    }
};

using LocalState = std::vector<LocalValue>;

struct LocalAnalysis {
    Cfg cfg;
    std::vector<uint8_t> escaped;
    std::vector<uint8_t> reached;
    std::vector<LocalState> in;
};

void transfer_local(const IRInst& inst, const RegConsts& consts,
                    const std::vector<uint8_t>& escaped, LocalState& state) {
    const auto* store = std::get_if<IRStoreLocal>(&inst);
    if (!store || store->local >= state.size() || escaped[store->local]) return;

    LocalValue v;
    if (consts.get_int(store->src, v.ival)) {
        v.tag = LocalValue::Int;
    } else if (consts.get_float(store->src, v.fval, v.fkind)) {
        v.tag = LocalValue::Float;
    } else {
        v.tag = LocalValue::Temp;
        v.reg = store->src;
    }
    state[store->local] = v;
}

void meet_local(LocalState& into, const LocalState& other) {
    for (size_t i = 0; i < into.size(); ++i) {
        if (!(into[i] == other[i])) into[i] = LocalValue{};
    }
}

LocalAnalysis analyze_locals(const IRFunction& fn, const RegConsts& consts) {
    LocalAnalysis a;
    a.cfg = build_cfg(fn.body);
    a.escaped = address_taken_locals(fn);
    a.reached.assign(a.cfg.count(), 0);
    a.in.assign(a.cfg.count(), LocalState(fn.local_count));
    if (a.cfg.count() == 0) return a;

    a.reached[0] = 1;
    bool changed = true;
    for (int iteration = 0; iteration < 256 && changed; ++iteration) {
        changed = false;
        for (uint32_t b = 0; b < a.cfg.count(); ++b) {
            if (!a.reached[b]) continue;
            LocalState out = a.in[b];
            for (uint32_t i = a.cfg.begin(b); i < a.cfg.end(b); ++i) {
                transfer_local(fn.body[i], consts, a.escaped, out);
            }
            for (uint32_t s : a.cfg.succ[b]) {
                if (!a.reached[s]) {
                    a.reached[s] = 1;
                    a.in[s] = out;
                    changed = true;
                    continue;
                }
                LocalState merged = a.in[s];
                meet_local(merged, out);
                if (merged != a.in[s]) {
                    a.in[s] = std::move(merged);
                    changed = true;
                }
            }
        }
    }
    return a;
}

// State right after body[index] has executed.
LocalState local_state_after(const LocalAnalysis& a, const IRFunction& fn,
                             const RegConsts& consts, size_t index) {
    const uint32_t b = a.cfg.block_of[index];
    if (!a.reached[b]) return LocalState(fn.local_count);
    LocalState state = a.in[b];
    for (uint32_t i = a.cfg.begin(b); i <= index; ++i) {
        transfer_local(fn.body[i], consts, a.escaped, state);
    }
    return state;
}

// Pass: constant / copy propagation through locals. Loads of a local with a
// known constant become constants; loads of a local holding a known temp are
// renamed to that temp; repeated loads inside one block share the first.

bool propagate_locals(IRFunction& fn) {
    if (fn.local_count == 0) return false;

    RegConsts consts(reg_capacity(fn));
    consts.scan(fn.body);
    const LocalAnalysis a = analyze_locals(fn, consts);

    RegRenamer rename;
    bool changed = false;
    std::vector<Reg> loaded(fn.local_count);

    for (uint32_t b = 0; b < a.cfg.count(); ++b) {
        if (!a.reached[b]) continue;
        LocalState state = a.in[b];
        std::fill(loaded.begin(), loaded.end(), kNone);

        for (uint32_t i = a.cfg.begin(b); i < a.cfg.end(b); ++i) {
            IRInst& inst = fn.body[i];
            if (const auto* load = std::get_if<IRLoadLocal>(&inst)) {
                const Local local = load->local;
                const Reg dst = load->dst;
                if (local >= state.size() || a.escaped[local]) continue;
                const LocalValue& v = state[local];
                switch (v.tag) {
                    case LocalValue::Int:
                        inst = IRLoadConst{dst, v.ival};
                        consts.set_int(dst, v.ival);
                        changed = true;
                        break;
                    case LocalValue::Float:
                        inst = IRLoadFloatConst{dst, v.fval, v.fkind};
                        consts.set_float(dst, v.fval, v.fkind);
                        changed = true;
                        break;
                    case LocalValue::Temp:
                        rename.add(dst, v.reg);
                        changed = true;
                        break;
                    case LocalValue::Unknown:
                        if (loaded[local] != kNone) {
                            rename.add(dst, loaded[local]);
                            changed = true;
                        } else {
                            loaded[local] = dst;
                        }
                        break;
                }
            } else if (const auto* store = std::get_if<IRStoreLocal>(&inst)) {
                if (store->local < loaded.size()) loaded[store->local] = kNone;
                transfer_local(inst, consts, a.escaped, state);
            }
        }
    }

    rename.apply(fn.body);
    return changed;
}

// Pass: control-flow cleanup. Jump threading through empty blocks, removal
// of unreachable blocks, of jumps to the next instruction and of labels
// nothing refers to

bool simplify_cfg(IRFunction& fn) {
    auto& body = fn.body;
    const size_t n = body.size();
    if (n == 0) return false;
    bool changed = false;

    LabelIndex labels = build_label_index(body);

    // A label followed (through other labels) by an unconditional jump can be
    // bypassed: jump straight to where that jump goes.
    const auto thread = [&](Label label) {
        for (int hop = 0; hop < 16; ++hop) {
            const auto it = labels.find(label);
            if (it == labels.end()) return label;
            size_t j = it->second + 1;
            while (j < n && std::holds_alternative<IRLabel>(body[j])) ++j;
            const auto* jump = j < n ? std::get_if<IRJump>(&body[j]) : nullptr;
            if (!jump || jump->target == label) return label;
            label = jump->target;
        }
        return label;
    };

    for (auto& inst : body) {
        if (auto* j = std::get_if<IRJump>(&inst)) {
            const Label t = thread(j->target);
            if (t != j->target) { j->target = t; changed = true; }
        } else if (auto* br = std::get_if<IRBranch>(&inst)) {
            const Label t = thread(br->then_label);
            const Label e = thread(br->else_label);
            if (t != br->then_label || e != br->else_label) {
                br->then_label = t;
                br->else_label = e;
                changed = true;
            }
            if (t == e) {
                inst = IRJump{t};
                changed = true;
            }
        }
    }

    std::vector<uint8_t> dead(n);
    {
        const Cfg cfg = build_cfg(body);
        const auto reached = reachable_blocks(cfg);
        for (uint32_t b = 0; b < cfg.count(); ++b) {
            if (reached[b]) continue;
            for (uint32_t i = cfg.begin(b); i < cfg.end(b); ++i) dead[i] = 1;
        }
    }

    // `jump L` directly followed by `L:` (possibly through other labels).
    for (size_t i = 0; i < n; ++i) {
        const auto* jump = std::get_if<IRJump>(&body[i]);
        if (!jump || dead[i]) continue;
        for (size_t j = i + 1; j < n; ++j) {
            const auto* label = std::get_if<IRLabel>(&body[j]);
            if (!label) break;
            if (label->id == jump->target) { dead[i] = 1; break; }
        }
    }

    std::unordered_set<Label> referenced;
    for (size_t i = 0; i < n; ++i) {
        if (dead[i]) continue;
        if (const auto* j = std::get_if<IRJump>(&body[i])) {
            referenced.insert(j->target);
        } else if (const auto* br = std::get_if<IRBranch>(&body[i])) {
            referenced.insert(br->then_label);
            referenced.insert(br->else_label);
        }
    }
    for (size_t i = 0; i < n; ++i) {
        const auto* label = std::get_if<IRLabel>(&body[i]);
        if (label && !referenced.count(label->id)) dead[i] = 1;
    }

    if (any_marked(dead)) {
        erase_marked(body, dead);
        changed = true;
    }
    return changed;
}

// Pass: compile-time loop evaluation. A region starts at a label that is
// entered only from the instruction right before it, evaluates a straight
// condition into a branch, and ends at that branch's else label. When the
// region contains nothing but constants, local loads/stores, integer
// arithmetic and control flow (nested loops, if/switch, break and continue
// included) it is interpreted with the local values known at its entry; if
// it terminates within the budget, the region is replaced by stores of the
// final values.

struct LoopRegion {
    size_t pre = 0;      // instruction the region is entered from
    size_t header = 0;   // IRLabel{header_label}
    size_t exit = 0;     // IRLabel{end}
    Label header_label = 0;
};

bool is_region_pure(const IRInst& inst) {
    return std::holds_alternative<IRLoadConst>(inst) ||
           std::holds_alternative<IRLoadLocal>(inst) ||
           std::holds_alternative<IRStoreLocal>(inst) ||
           std::holds_alternative<IRBinary>(inst) ||
           std::holds_alternative<IRCast>(inst) ||
           std::holds_alternative<IRLabel>(inst) ||
           std::holds_alternative<IRJump>(inst) ||
           std::holds_alternative<IRBranch>(inst);
}

// Instruction indices that jump to each label.
using LabelRefs = std::unordered_map<Label, std::vector<size_t>>;

LabelRefs build_label_refs(const std::vector<IRInst>& body) {
    LabelRefs refs;
    for (size_t i = 0; i < body.size(); ++i) {
        if (const auto* j = std::get_if<IRJump>(&body[i])) {
            refs[j->target].push_back(i);
        } else if (const auto* br = std::get_if<IRBranch>(&body[i])) {
            refs[br->then_label].push_back(i);
            if (br->else_label != br->then_label) refs[br->else_label].push_back(i);
        }
    }
    return refs;
}

bool find_loop_region(const std::vector<IRInst>& body, size_t header,
                      const LabelIndex& labels, const LabelRefs& refs, LoopRegion& out) {
    const size_t n = body.size();
    const auto* label = std::get_if<IRLabel>(&body[header]);
    if (!label || header == 0) return false;

    // Entered by falling through from the previous instruction or by a jump
    // there; anything else means the entry state is a merge we do not track.
    const size_t pre = header - 1;
    if (const auto* j = std::get_if<IRJump>(&body[pre])) {
        if (j->target != label->id) return false;
    } else if (is_terminator(body[pre])) {
        return false;
    }

    size_t branch = header + 1;
    for (; branch < n; ++branch) {
        const IRInst& inst = body[branch];
        if (std::holds_alternative<IRBranch>(inst)) break;
        if (is_terminator(inst) || std::holds_alternative<IRLabel>(inst)) return false;
    }
    if (branch >= n) return false;

    const auto exit_it = labels.find(std::get<IRBranch>(body[branch]).else_label);
    if (exit_it == labels.end() || exit_it->second <= branch) return false;
    const size_t exit = exit_it->second;

    const auto inside = [&](Label target) {
        const auto it = labels.find(target);
        return it != labels.end() && it->second >= header && it->second <= exit;
    };
    for (size_t i = header + 1; i < exit; ++i) {
        const IRInst& inst = body[i];
        if (!is_region_pure(inst)) return false;
        if (const auto* j = std::get_if<IRJump>(&inst)) {
            if (!inside(j->target)) return false;
        } else if (const auto* br = std::get_if<IRBranch>(&inst)) {
            if (!inside(br->then_label) || !inside(br->else_label)) return false;
        }
    }

    // No control flow from outside [pre, exit) may land inside the region.
    for (size_t i = header; i < exit; ++i) {
        const auto* l = std::get_if<IRLabel>(&body[i]);
        if (!l) continue;
        const auto it = refs.find(l->id);
        if (it == refs.end()) continue;
        for (size_t from : it->second) {
            if (from < pre || from >= exit) return false;
        }
    }

    out.pre = pre;
    out.header = header;
    out.exit = exit;
    out.header_label = label->id;
    return true;
}

struct LoopInterp {
    std::vector<int64_t> reg;
    std::vector<uint8_t> reg_ok;
    std::vector<int64_t> loc;
    std::vector<uint8_t> loc_ok;
    std::vector<uint8_t> loc_written;
    bool budget_exhausted = false;
};

// The region is compiled to a flat op list first: labels vanish, jump
// targets become op indices and constant temps are preloaded, so the hot
// loop is a plain switch over dense arrays.
struct LoopOp {
    enum Kind : uint8_t { Load, Store, Bin, Cast, Jump, Branch, Exit } kind = Exit;
    IRBinaryOp op = IRBinaryOp::Add;
    TypeKind type = TypeKind::I32;
    TypeKind type2 = TypeKind::I32;
    ast::CastKind cast = ast::CastKind::ValueCast;
    uint32_t dst = 0;
    uint32_t a = 0;
    uint32_t b = 0;
    uint32_t target = 0;
    uint32_t target2 = 0;
};

std::vector<LoopOp> compile_loop_region(const std::vector<IRInst>& body, const LoopRegion& region) {
    std::vector<LoopOp> ops;
    std::unordered_map<Label, uint32_t> label_op;

    for (size_t i = region.header; i <= region.exit; ++i) {
        LoopOp op;
        bool emit = true;
        bool pure = true;
        std::visit(Overloaded{
            [&](const IRLabel& x) {
                label_op[x.id] = static_cast<uint32_t>(ops.size());
                emit = i == region.exit;   // the exit label becomes the Exit op
            },
            [&](const IRLoadConst&) { emit = false; },   // preloaded from RegConsts
            [&](const IRLoadLocal& x) {
                op.kind = LoopOp::Load; op.dst = x.dst; op.a = x.local;
            },
            [&](const IRStoreLocal& x) {
                op.kind = LoopOp::Store; op.dst = x.local; op.a = x.src;
            },
            [&](const IRBinary& x) {
                op.kind = LoopOp::Bin; op.op = x.op; op.type = x.type_kind;
                op.dst = x.dst; op.a = x.lhs; op.b = x.rhs;
            },
            [&](const IRCast& x) {
                op.kind = LoopOp::Cast; op.dst = x.dst; op.a = x.src;
                op.type = x.src_kind; op.type2 = x.target_kind; op.cast = x.kind;
            },
            [&](const IRJump& x) {
                op.kind = LoopOp::Jump; op.target = x.target;
            },
            [&](const IRBranch& x) {
                op.kind = LoopOp::Branch; op.a = x.cond;
                op.target = x.then_label; op.target2 = x.else_label;
            },
            [&](const auto&) { pure = false; }
        }, body[i]);
        if (!pure) return {};
        if (emit) ops.push_back(op);
    }

    for (auto& op : ops) {
        if (op.kind != LoopOp::Jump && op.kind != LoopOp::Branch) continue;
        const auto t = label_op.find(op.target);
        if (t == label_op.end()) return {};
        op.target = t->second;
        if (op.kind == LoopOp::Branch) {
            const auto e = label_op.find(op.target2);
            if (e == label_op.end()) return {};
            op.target2 = e->second;
        }
    }
    return ops;
}

bool run_loop_region(const std::vector<LoopOp>& ops, LoopInterp& st, int64_t budget) {
    if (ops.empty()) return false;
    const size_t regs = st.reg.size();
    const size_t locals = st.loc.size();

    for (const auto& op : ops) {
        const bool regs_ok = (op.kind == LoopOp::Load  && op.dst < regs && op.a < locals) ||
                             (op.kind == LoopOp::Store && op.dst < locals && op.a < regs) ||
                             (op.kind == LoopOp::Bin   && op.dst < regs && op.a < regs && op.b < regs) ||
                             (op.kind == LoopOp::Cast  && op.dst < regs && op.a < regs) ||
                             (op.kind == LoopOp::Branch && op.a < regs) ||
                             op.kind == LoopOp::Jump || op.kind == LoopOp::Exit;
        if (!regs_ok) return false;
    }

    size_t pc = 0;
    for (;;) {
        if (--budget < 0) {
            st.budget_exhausted = true;
            return false;
        }
        const LoopOp& op = ops[pc];
        switch (op.kind) {
            case LoopOp::Load:
                if (!st.loc_ok[op.a]) return false;
                st.reg[op.dst] = st.loc[op.a];
                st.reg_ok[op.dst] = 1;
                ++pc;
                break;
            case LoopOp::Store:
                if (!st.reg_ok[op.a]) return false;
                st.loc[op.dst] = st.reg[op.a];
                st.loc_ok[op.dst] = 1;
                st.loc_written[op.dst] = 1;
                ++pc;
                break;
            case LoopOp::Bin: {
                if (!st.reg_ok[op.a] || !st.reg_ok[op.b]) return false;
                const auto v = fold_int(op.op, st.reg[op.a], st.reg[op.b], op.type);
                if (!v) return false;
                st.reg[op.dst] = *v;
                st.reg_ok[op.dst] = 1;
                ++pc;
                break;
            }
            case LoopOp::Cast: {
                if (!st.reg_ok[op.a]) return false;
                const IRCast cast{op.dst, op.a, op.type, op.type2, op.cast};
                const auto v = fold_cast(cast, st.reg[op.a]);
                if (!v) return false;
                st.reg[op.dst] = *v;
                st.reg_ok[op.dst] = 1;
                ++pc;
                break;
            }
            case LoopOp::Jump:
                pc = op.target;
                break;
            case LoopOp::Branch:
                if (!st.reg_ok[op.a]) return false;
                pc = st.reg[op.a] != 0 ? op.target : op.target2;
                break;
            case LoopOp::Exit:
                return true;
        }
    }
}

bool fold_loops(IRFunction& fn, int64_t budget, std::unordered_set<Label>& exhausted,
                OptStats* stats) {
    bool changed = false;

    for (bool retry = true; retry;) {
        retry = false;
        if (fn.body.size() < 4) break;

        RegConsts consts(reg_capacity(fn));
        consts.scan(fn.body);
        const LabelIndex labels = build_label_index(fn.body);
        const LabelRefs refs = build_label_refs(fn.body);
        std::optional<LocalAnalysis> analysis;

        for (size_t i = 1; i + 1 < fn.body.size(); ++i) {
            LoopRegion region;
            if (!find_loop_region(fn.body, i, labels, refs, region)) continue;
            if (exhausted.count(region.header_label)) continue;
            if (!analysis) analysis = analyze_locals(fn, consts);

            LoopInterp st;
            st.reg.assign(consts.tag.size(), 0);
            st.reg_ok.assign(consts.tag.size(), 0);
            for (size_t r = 0; r < consts.tag.size(); ++r) {
                if (consts.tag[r] == RegConsts::Int) {
                    st.reg[r] = consts.ival[r];
                    st.reg_ok[r] = 1;
                }
            }
            st.loc.assign(fn.local_count, 0);
            st.loc_ok.assign(fn.local_count, 0);
            st.loc_written.assign(fn.local_count, 0);
            const LocalState init = local_state_after(*analysis, fn, consts, region.pre);
            for (size_t l = 0; l < init.size(); ++l) {
                if (init[l].tag == LocalValue::Int) {
                    st.loc[l] = init[l].ival;
                    st.loc_ok[l] = 1;
                }
            }

            if (!run_loop_region(compile_loop_region(fn.body, region), st, budget)) {
                if (st.budget_exhausted) exhausted.insert(region.header_label);
                continue;
            }

            // Keep the header and exit labels; the cfg pass drops them once
            // nothing refers to them any more.
            std::vector<IRInst> body;
            body.reserve(fn.body.size());
            for (size_t k = 0; k <= region.header; ++k) body.push_back(std::move(fn.body[k]));
            for (Local l = 0; l < fn.local_count; ++l) {
                if (!st.loc_written[l]) continue;
                const Reg tmp = fn.temp_count++;
                body.emplace_back(IRLoadConst{tmp, st.loc[l]});
                body.emplace_back(IRStoreLocal{l, tmp});
            }
            for (size_t k = region.exit; k < fn.body.size(); ++k) body.push_back(std::move(fn.body[k]));
            fn.body = std::move(body);

            if (stats) ++stats->loops_folded;
            changed = true;
            retry = true;
            break;
        }
    }
    return changed;
}

// Pass: dead temp elimination. Temps are single-assignment and every use
// follows its definition in program order, so one backward sweep suffices

bool remove_dead_instructions(IRFunction& fn) {
    const size_t n = fn.body.size();
    if (n == 0) return false;

    std::vector<uint8_t> live(reg_capacity(fn));
    std::vector<uint8_t> dead(n);

    for (size_t i = n; i-- > 0;) {
        const IRInst& inst = fn.body[i];
        const auto def = def_of(inst);
        if (def && !has_side_effect(inst) && !live[*def]) {
            dead[i] = 1;
            continue;
        }
        for_each_use(inst, [&](Reg use) { if (use < live.size()) live[use] = 1; });
    }

    if (!any_marked(dead)) return false;
    erase_marked(fn.body, dead);
    return true;
}

// Pass dead store elimination for locals, via block-level liveness. Stores
// to address-taken locals are always kept

bool remove_dead_locals(IRFunction& fn) {
    const size_t n = fn.body.size();
    const size_t locals = fn.local_count;
    if (n == 0 || locals == 0) return false;

    const auto escaped = address_taken_locals(fn);
    const Cfg cfg = build_cfg(fn.body);
    const uint32_t blocks = cfg.count();

    std::vector<BitSet> gen(blocks, BitSet(locals));
    std::vector<BitSet> kill(blocks, BitSet(locals));
    for (uint32_t b = 0; b < blocks; ++b) {
        for (uint32_t i = cfg.begin(b); i < cfg.end(b); ++i) {
            const IRInst& inst = fn.body[i];
            for_each_local_use(inst, [&](Local l) {
                if (l < locals && !kill[b].test(l)) gen[b].set(l);
            });
            if (const auto* s = std::get_if<IRStoreLocal>(&inst)) {
                if (s->local < locals && !escaped[s->local]) kill[b].set(s->local);
            }
        }
    }

    std::vector<BitSet> live_in(blocks, BitSet(locals));
    std::vector<BitSet> live_out(blocks, BitSet(locals));
    bool changed = true;
    for (int iteration = 0; iteration < 512 && changed; ++iteration) {
        changed = false;
        for (uint32_t b = blocks; b-- > 0;) {
            BitSet out(locals);
            for (uint32_t s : cfg.succ[b]) out.merge(live_in[s]);
            BitSet in = gen[b];
            for (size_t w = 0; w < in.words.size(); ++w) {
                in.words[w] |= out.words[w] & ~kill[b].words[w];
            }
            live_out[b] = out;
            if (!(in == live_in[b])) {
                live_in[b] = std::move(in);
                changed = true;
            }
        }
    }

    std::vector<uint8_t> dead(n);
    for (uint32_t b = 0; b < blocks; ++b) {
        BitSet live = live_out[b];
        for (uint32_t i = cfg.end(b); i-- > cfg.begin(b);) {
            const IRInst& inst = fn.body[i];
            if (const auto* s = std::get_if<IRStoreLocal>(&inst)) {
                if (s->local >= locals || escaped[s->local]) continue;
                if (live.test(s->local)) live.reset(s->local);
                else dead[i] = 1;
                continue;
            }
            for_each_local_use(inst, [&](Local l) { if (l < locals) live.set(l); });
        }
    }

    if (!any_marked(dead)) return false;
    erase_marked(fn.body, dead);
    return true;
}

// Whole-program passes: drop unreferenced functions, strings and globals and
// renumber the survivors

template <class T>
std::vector<uint32_t> compact_items(std::vector<T>& items, const std::vector<uint8_t>& used) {
    if (std::all_of(used.begin(), used.end(), [](uint8_t u) { return u != 0; })) return {};

    std::vector<uint32_t> remap(items.size(), kNone);
    std::vector<T> kept;
    kept.reserve(items.size());
    for (size_t i = 0; i < items.size(); ++i) {
        if (!used[i]) continue;
        remap[i] = static_cast<uint32_t>(kept.size());
        kept.push_back(std::move(items[i]));
    }
    items = std::move(kept);
    return remap;
}

template <class F>
void for_each_inst(IRProgram& program, F&& f) {
    for (auto& fn : program.functions) {
        for (auto& inst : fn.body) f(inst);
    }
}

bool is_entry_root(const IRFunction& fn) {
    return fn.is_entry ||
           fn.is_extern ||
           fn.syscall_number >= 0 ||
           !fn.export_name.empty() ||
           !fn.import_dll.empty() ||
           fn.name.ends_with("main");
}

void remove_unused_functions(IRProgram& program) {
    const size_t size = program.functions.size();
    std::vector<uint8_t> keep(size);
    std::vector<size_t> work;

    for (size_t i = 0; i < size; ++i) {
        if (!is_entry_root(program.functions[i])) continue;
        keep[i] = 1;
        work.push_back(i);
    }
    while (!work.empty()) {
        const size_t index = work.back();
        work.pop_back();
        for (const auto& inst : program.functions[index].body) {
            const auto* call = std::get_if<IRCall>(&inst);
            if (call && call->func_id < size && !keep[call->func_id]) {
                keep[call->func_id] = 1;
                work.push_back(call->func_id);
            }
        }
    }

    const auto remap = compact_items(program.functions, keep);
    if (remap.empty()) return;
    for (size_t i = 0; i < program.functions.size(); ++i) {
        program.functions[i].id = static_cast<uint32_t>(i);
    }
    for_each_inst(program, [&](IRInst& inst) {
        if (auto* call = std::get_if<IRCall>(&inst)) call->func_id = remap[call->func_id];
    });
}

void compact_strings(IRProgram& program) {
    std::vector<uint8_t> used(program.strings.size());
    for_each_inst(program, [&](const IRInst& inst) {
        const auto* load = std::get_if<IRLoadString>(&inst);
        if (load && load->string_id < used.size()) used[load->string_id] = 1;
    });

    const auto remap = compact_items(program.strings, used);
    if (remap.empty()) return;
    for (size_t i = 0; i < program.strings.size(); ++i) {
        program.strings[i].id = static_cast<uint32_t>(i);
    }
    for_each_inst(program, [&](IRInst& inst) {
        if (auto* load = std::get_if<IRLoadString>(&inst)) load->string_id = remap[load->string_id];
    });
}

void compact_globals(IRProgram& program) {
    std::vector<uint8_t> used(program.globals.size());
    const auto mark = [&](uint32_t id) { if (id < used.size()) used[id] = 1; };
    for_each_inst(program, [&](const IRInst& inst) {
        std::visit(Overloaded{
            [&](const IRLoadGlobal& x)     { mark(x.global_id); },
            [&](const IRStoreGlobal& x)    { mark(x.global_id); },
            [&](const IRLoadGlobalAddr& x) { mark(x.global_id); },
            [](const auto&) {}
        }, inst);
    });

    const auto remap = compact_items(program.globals, used);
    if (remap.empty()) return;
    for_each_inst(program, [&](IRInst& inst) {
        std::visit(Overloaded{
            [&](IRLoadGlobal& x)     { x.global_id = remap[x.global_id]; },
            [&](IRStoreGlobal& x)    { x.global_id = remap[x.global_id]; },
            [&](IRLoadGlobalAddr& x) { x.global_id = remap[x.global_id]; },
            [](auto&) {}
        }, inst);
    });
}

// Driver

size_t count_insts(const IRProgram& program) {
    size_t total = 0;
    for (const auto& fn : program.functions) total += fn.body.size();
    return total;
}

struct PassRunner {
    OptStats* stats;

    template <class F>
    bool run(const char* name, F&& pass) {
        if (skip_pass(name)) return false;
        if (!stats) return pass();
        const auto start = std::chrono::steady_clock::now();
        const bool changed = pass();
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count();
        auto& p = stats->pass(name);
        p.nanoseconds += static_cast<uint64_t>(ns);
        ++p.invocations;
        if (changed) ++p.changes;
        return changed;
    }
};

} // namespace

void optimize(IRProgram& program, OptLevel level, bool keep_all_functions, OptStats* stats) {
    if (stats) {
        stats->insts_before = count_insts(program);
        stats->functions_before = program.functions.size();
    }
    if (level == OptLevel::O0) {
        if (stats) {
            stats->insts_after = stats->insts_before;
            stats->functions_after = stats->functions_before;
        }
        return;
    }

    const int lvl = static_cast<int>(level);
    const int64_t loop_budget = lvl >= 3 ? kLoopBudgetO3 : kLoopBudgetO2;
    const bool branches = !skip_pass("branch");
    PassRunner runner{stats};

    for (auto& fn : program.functions) {
        if (fn.is_extern || fn.body.empty()) continue;
        std::unordered_set<Label> exhausted_loops;

        for (int round = 0; round < kMaxRounds; ++round) {
            bool changed = false;
            changed |= runner.run("fold",   [&] { return fold_constants(fn, branches, stats); });
            changed |= runner.run("locals", [&] { return propagate_locals(fn); });
            changed |= runner.run("cfg",    [&] { return simplify_cfg(fn); });
            if (lvl >= 2) {
                changed |= runner.run("loop", [&] {
                    return fold_loops(fn, loop_budget, exhausted_loops, stats);
                });
            }
            changed |= runner.run("dead",   [&] { return remove_dead_instructions(fn); });
            changed |= runner.run("dse",    [&] { return remove_dead_locals(fn); });
            if (!changed) break;
        }
    }

    if (lvl >= 2) {
        runner.run("prune", [&] {
            if (!keep_all_functions) remove_unused_functions(program);
            compact_strings(program);
            compact_globals(program);
            return true;
        });
    }

    if (stats) {
        stats->insts_after = count_insts(program);
        stats->functions_after = program.functions.size();
    }
}

} // namespace quant::codegen

#pragma once

#include <cstdint>
#include <vector>
#include <variant>
#include <string>

#include "quant/frontend/ast.h"

namespace quant::codegen {

using Reg = uint32_t;
using Local = uint32_t;
using Label = uint32_t;

enum class IRBinaryOp {
    Add, Sub, Mul, Div,
    Eq, NotEq,
    Lt, Lte, Gt, Gte,
    BitAnd, BitOr,
    LogicAnd, LogicOr
};

struct IRLoadConst {
    Reg dst;
    int64_t value;
};
struct IRLoadFloatConst {
    Reg dst;
    double value;
    ast::TypeKind kind;   // F32 or F64
};
struct IRLoadString {
    Reg dst;
    uint32_t string_id;
};

struct IRLoadLocal {
    Reg dst;
    Local local;
};

struct IRStoreLocal {
    Local local;
    Reg src;
};

struct IRAddrOf {
    Reg dst;
    Local local;
};

struct IRBinary {
    IRBinaryOp op;
    Reg dst;
    Reg lhs;
    Reg rhs;
    ast::TypeKind type_kind = ast::TypeKind::I32;
};

struct IRCall {
    Reg dst;
    uint32_t func_id;
    std::vector<Reg> args;
    bool sret = false;   // struct return: first arg is hidden pointer to result buffer
};
struct IRCast {
    Reg dst;
    Reg src;
    ast::TypeKind src_kind;
    ast::TypeKind target_kind;
    ast::CastKind kind;
};

struct IRShift {
    Reg dst;
    Reg lhs;
    Reg rhs;

    bool is_left;
    bool is_ar;
};

struct IRReturn {
    Reg value;
};

struct IRJump {
    Label target;
};

struct IRBranch {
    Reg cond;
    Label then_label;
    Label else_label;
};

struct IRLabel {
    Label id;
};

struct IRGetField {
    Reg dst;
    Reg base;
    uint32_t offset;
};

struct IRSetField {
    Reg base;
    Reg value;
    uint32_t offset;
};
struct IRString {
    Label id;
    std::string value;
};

struct IRGlobal {
    std::string name;
    uint32_t size;   // bytes in .data (8-byte aligned)
    bool is_extern = false;  // resolved from .a archive, not emitted in .o
};

struct IRRegionBegin {
    Local region_local;  // local slot holding pointer to Region struct
    uint32_t region_size;
};

struct IRRegionAlloc {
    Reg dst;
    Reg size;
    Local region_local;  // local slot holding pointer to Region struct
};

struct IRRegionEnd {
    Local region_local;  // local slot holding pointer to Region struct
};


struct IRAlloca {
    Reg dst;
    uint32_t offset; // cumulative byte offset from rbp (computed at IR gen time)
};

struct IRLoadGlobal {
    Reg dst;
    uint32_t global_id;
};

struct IRStoreGlobal {
    uint32_t global_id;
    Reg src;
};

struct IRLoadGlobalAddr {
    Reg dst;
    uint32_t global_id;
};

struct IRLoadElement {
    Reg dst;
    Reg base;
    Reg index;
    uint32_t elem_size;
};

struct IRStoreElement {
    Reg base;
    Reg index;
    Reg value;
    uint32_t elem_size;
};

using IRInst = std::variant<
    IRLoadConst,
    IRLoadFloatConst,
    IRLoadLocal,
    IRStoreLocal,
    IRAddrOf,
    IRBinary,
    IRCall,
    IRReturn,
    IRJump,
    IRBranch,
    IRLabel,
    IRGetField,
    IRSetField,
    IRCast,
    IRLoadString,
    IRLoadElement,
    IRStoreElement,
    IRRegionBegin,
    IRRegionAlloc,
    IRRegionEnd,
    IRAlloca,
    IRLoadGlobal,
    IRStoreGlobal,
    IRShift,
    IRLoadGlobalAddr
>;

struct IRFunction {
    uint32_t id;
    std::string name;

    uint32_t arg_count = 0;
    uint32_t local_count = 0;
    uint32_t temp_count = 0;
    uint32_t extra_stack = 0;

    std::vector<IRInst> body;

    bool is_extern = false;
    bool is_entry = false;
    bool sret = false;   // returns struct via hidden pointer arg
    int64_t syscall_number = -1;   // >= 0 for extern functions lowered to a raw syscall
    std::string export_name;       // overrides the asm symbol name (@export)
    std::string import_dll;        // non-empty: extern function is imported from this DLL (@import)
    std::string import_name;       // imported symbol name (defaults to the function name)
};

struct IRProgram {
    std::vector<IRFunction> functions;
    std::vector<IRString> strings;
    std::vector<IRGlobal> globals;

    void dump() const;
};

} // namespace quant::codegen

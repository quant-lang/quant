#include "quant/frontend/ast.h"

#include "quant/support/compiler_context.h"

using namespace ast;

std::string Type::to_string(quant::CompilerContext& ctx) const
    {
        switch (kind)
        {
        case TypeKind::Void:      return "void";
        case TypeKind::Bool:      return "bool";
    
        case TypeKind::I8:        return "i8";
        case TypeKind::I16:       return "i16";
        case TypeKind::I32:       return "i32";
        case TypeKind::I64:       return "i64";
    
        case TypeKind::U8:        return "u8";
        case TypeKind::U16:       return "u16";
        case TypeKind::U32:       return "u32";
        case TypeKind::U64:       return "u64";
    
        case TypeKind::F32:       return "f32";
        case TypeKind::F64:       return "f64";

        case TypeKind::Struct:
            {
                std::string base = struct_name;
                std::string unmangled;

                if (ctx.types.is_mangled_name(base, unmangled)) {
                    base = unmangled;
                } else {
                    auto dollar = base.find('$');
                    if (dollar != std::string::npos) {
                        base = base.substr(0, dollar);
                    }
                }

                if (!type_args.empty()) {
                    std::ostringstream oss;
                    bool is_first = true;

                    for (const auto& arg : type_args) {
                        if (!is_first) {
                            oss << ", ";
                        }
                        is_first = false;
                        oss << arg->to_string(ctx);
                    }
                    
                    base += "<" + oss.str() + ">";
                }

                return base;
            }

        case TypeKind::Pointer:   return "*" + pointed->to_string(ctx);
        case TypeKind::Reference: return "&" + pointed->to_string(ctx);
        case TypeKind::NullPtr:   return "nullptr";
        case TypeKind::Generic:   return "generic";
    
        case TypeKind::Count:     return "count";
        }
        return "unknown";
    }
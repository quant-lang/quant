#pragma once
#include <unordered_map>
#include <cstdint>
#include <vector>
#include <string>

namespace quant::attrs{
    enum class AttributeTarget : uint32_t {
        None      = 0,
    	Function  = 1 << 0,
    	Variable  = 1 << 1,
		Field 	  = 1 << 2,
   		Struct    = 1 << 3,
    	Module    = 1 << 4,
	};
	inline AttributeTarget operator|(AttributeTarget a, AttributeTarget b) {
    	return static_cast<AttributeTarget>(
        	static_cast<uint32_t>(a) |
        	static_cast<uint32_t>(b)
    	);
	}
	struct LoweringArg{
	    enum class Source {VarName, AttrExpr, Literal};
		Source source;
		uint32_t attr_arg_index = 0;
		std::string literal;
	};
	struct AttributeInfo {
    	std::string name;
        std::string lowering_fn;
        std::vector<LoweringArg> lowering_args;
    	AttributeTarget targets;
    	uint32_t min_args;
    	uint32_t max_args;
	};
	inline std::unordered_map<std::string, AttributeInfo> attributes = {
    	{ "entry", { "entry", std::string(), {}, AttributeTarget::Function, 0, 0 }},
		{ "init",  { "init", std::string(), {}, AttributeTarget::Variable | AttributeTarget::Field, 0, 0 }},
		{ "guard", { "guard", std::string(), {},
		                                AttributeTarget::Variable | AttributeTarget::Field, 1, 1 }},
		{ "public", { "public", std::string(),{},AttributeTarget::Function | AttributeTarget::Variable
                        | AttributeTarget::Field | AttributeTarget::Struct, 0, 0}},
		{ "private", { "private", std::string(), {}, AttributeTarget::Function | AttributeTarget::Variable
                         | AttributeTarget::Field | AttributeTarget::Struct, 0, 0}},
	{ "hide", {"hide", std::string(),{}, AttributeTarget::Function | AttributeTarget::Variable
                    | AttributeTarget::Struct | AttributeTarget::Module, 0, 0}},
	{ "unhide", {"unhide", std::string(),{}, AttributeTarget::Function | AttributeTarget::Variable
                      | AttributeTarget::Struct | AttributeTarget::Module, 0, 0}},
	{ "syscall", {"syscall", std::string(),{}, AttributeTarget::Function, 1, 1}},
		{ "export", {"export", std::string(),{}, AttributeTarget::Function, 1, 1}},
	{ "import", {"import", std::string(),{}, AttributeTarget::Function, 1, 2}}
	};
}

#pragma once
#include "quant/backend/x86_64.h"
#include "quant/backend/aarch_64.h"
#include "quant/backend/symbol_table.h"

namespace quant::codegen {

class CodeEmitter {
    public:
        CodeEmitter(x86::Emitter& e, mc::Object& o, mc::SymbolTable& s);
        CodeEmitter(aarch64::Emitter& e, mc::Object& o, mc::SymbolTable& s);

        x86::Emitter&     x86();      // crash, if the emitter is not the right arch
        aarch64::Emitter& aarch64();  // crash, if the emitter is not the right arch
        mc::Object&       object();
        mc::SymbolTable&  symbols();
    private:
        void* emitter_;
        mc::Object* object_;
        mc::SymbolTable* symbols_;
        mc::TargetArch arch_;
};

} // namespace quant::codegen

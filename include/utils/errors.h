#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdio>
#include "utils/logger.h"
#include "utils/colors.h"

struct SourceFile {
    std::vector<std::string> lines;
};

class ErrorBag {
    std::unordered_map<std::string, SourceFile> sources;
    size_t error_count = 0;

    // Location used by legacy `add(message)` call sites that do not pass a
    // node explicitly. Semantic analysis and IR generation set this while
    // walking a statement/expression, so every diagnostic still points at the
    // offending source token instead of printing a bare `error: ...`.
    SourceLocation fallback_loc;

    void print_carets(const SourceLocation& loc, int length, const std::string& line);
    void print(const SourceLocation& loc, int length, const std::string& msg);

public:
    class ParserException : public std::exception {};

    void add_source(const std::string& path, SourceFile sf);

    void add(const SourceLocation& loc, int length, const std::string& msg);
    void add(const SourceLocation& loc, const std::string& msg);
    void add(const std::string& msg);

    void set_fallback_location(const SourceLocation& loc) { fallback_loc = loc; }
    const SourceLocation& fallback_location() const { return fallback_loc; }

    bool has_errors() const { return error_count > 0; }
    size_t count() const { return error_count; }
};

// Restores the previous fallback location on scope exit, so nested
// expressions/statements do not leak their location into their parent.
class ErrorLocationScope {
    ErrorBag& errors;
    SourceLocation previous;

public:
    ErrorLocationScope(ErrorBag& bag, const SourceLocation& loc)
        : errors(bag), previous(bag.fallback_location()) {
        if (loc.line > 0) {
            errors.set_fallback_location(loc);
        }
    }

    ~ErrorLocationScope() {
        errors.set_fallback_location(previous);
    }

    ErrorLocationScope(const ErrorLocationScope&) = delete;
    ErrorLocationScope& operator=(const ErrorLocationScope&) = delete;
};

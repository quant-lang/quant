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

    void print_carets(const SourceLocation& loc, int length, const std::string& line);

public:
    class ParserException : public std::exception {};

    void add_source(const std::string& path, SourceFile sf);

    void add(const SourceLocation& loc, int length, const std::string& msg);
    void add(const SourceLocation& loc, const std::string& msg);
    void add(const std::string& msg);

    bool has_errors() const { return error_count > 0; }
    size_t count() const { return error_count; }
};

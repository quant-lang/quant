#include "utils/errors.h"

#include <cctype>

using namespace utils::colors;

namespace {

bool valid_location(const SourceLocation& loc) {
    return loc.line > 0 && loc.column > 0 && loc.file != "<unknown>";
}

// A source location without an explicit length still identifies a token.
// Use the identifier/number starting at the column so the caret is useful.
int token_length(const std::string& line, const SourceLocation& loc, int length) {
    if (length > 0) return length;
    if (!valid_location(loc) || (size_t)(loc.column - 1) >= line.size()) return 1;

    size_t pos = (size_t)(loc.column - 1);
    size_t end = pos;
    while (end < line.size()) {
        unsigned char c = (unsigned char)line[end];
        if (!(std::isalnum(c) || c == '_' || c == ':' || c == '@' || c == '.')) break;
        ++end;
    }
    return end > pos ? (int)(end - pos) : 1;
}

} // namespace

void ErrorBag::add_source(const std::string& path, SourceFile sf) {
    sources[path] = std::move(sf);
}

void ErrorBag::print_carets(const SourceLocation& loc, int length, const std::string& line) {
    fprintf(stderr, "    %s\n", line.c_str());
    fprintf(stderr, "    ");

    for (int i = 0; i < loc.column - 1; i++) {
        fputc('~', stderr);
    }

    set_color(RED);
    for (int i = 0; i < length; i++) {
        fputc('^', stderr);
    }
    reset_color();

    int end_pos = loc.column - 1 + length;
    for (int i = end_pos; i < (int)line.size(); i++) {
        fputc('~', stderr);
    }
    fputc('\n', stderr);
}

void ErrorBag::print(const SourceLocation& loc, int length, const std::string& msg) {
    error_count++;

    set_color(RED);
    fprintf(stderr, "%s:%d:%d: error: %s\n",
            loc.file.c_str(), loc.line, loc.column, msg.c_str());
    reset_color();

    auto it = sources.find(loc.file);
    if (it != sources.end() && loc.line > 0 && (size_t)loc.line <= it->second.lines.size()) {
        const std::string& line = it->second.lines[loc.line - 1];
        print_carets(loc, token_length(line, loc, length), line);
    }
}

void ErrorBag::add(const SourceLocation& loc, int length, const std::string& msg) {
    if (valid_location(loc)) {
        print(loc, length, msg);
        return;
    }
    if (valid_location(fallback_loc)) {
        print(fallback_loc, fallback_loc.length, msg);
        return;
    }

    // Malformed source location: report the message without a snippet rather
    // than fabricating a `<unknown>:0:0` position.
    error_count++;
    set_color(RED);
    fprintf(stderr, "error: %s\n", msg.c_str());
    reset_color();
}

void ErrorBag::add(const SourceLocation& loc, const std::string& msg) {
    add(loc, loc.length, msg);
}

void ErrorBag::add(const std::string& msg) {
    add(SourceLocation{}, 0, msg);
}

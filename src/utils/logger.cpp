#include <iostream>

#include "utils/logger.h"
#include "utils/colors.h"

using namespace utils::colors;
namespace utils::logger {

namespace {

bool has_location(const SourceLocation& loc) {
    return loc.line > 0 && loc.file != "<unknown>";
}

void print_location(std::ostream& out, const SourceLocation& loc) {
    if (has_location(loc)) {
        out << loc.file << ":" << loc.line << ":" << loc.column << ": ";
    }
}

} // namespace

// diagnostics (WITH location)
// Keep the same `location: severity: message` shape as ErrorBag so every
// compiler diagnostic is rendered in one format.
void info(const SourceLocation& loc, const std::string& msg) {
    print_location(std::cout, loc);
    std::cout << "info: " << msg << "\n";
}

void warn(const SourceLocation& loc, const std::string& msg) {
    set_color(YELLOW);
    print_location(std::cout, loc);
    std::cout << "warning: " << msg << "\n";
    reset_color();
}

void error(const SourceLocation& loc, const std::string& msg) {
    set_color(RED);
    print_location(std::cerr, loc);
    std::cerr << "error: " << msg << "\n";
    reset_color();
}

[[noreturn]] void fatal(const SourceLocation& loc, const std::string& msg) {
    error(loc, msg);
    std::exit(1);
}

// system logs (WITHOUT location)

void info(const std::string& msg) {
    std::cout << "info: " << msg << "\n";
}

void warn(const std::string& msg) {
    set_color(YELLOW);
    std::cout << "warning: " << msg << "\n";
    reset_color();
}

void error(const std::string& msg) {
    set_color(RED);
    std::cerr << "error: " << msg << "\n";
    reset_color();
}

[[noreturn]] void fatal(const std::string& msg) {
    error(msg);
    std::exit(1);
}
[[noreturn]] void crash(const std::string& msg){
    throw std::runtime_error(msg);
}
[[noreturn]] void crash(const SourceLocation& loc, const std::string& msg) {
    std::ostringstream oss;

    if (loc.line >= 0) {
        oss << loc.file << ":" << loc.line << ":" << loc.column << " ";
    }
    oss << msg;

    throw std::runtime_error(oss.str());
}
}

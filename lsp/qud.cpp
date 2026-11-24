#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

#include "quant/driver/pipeline.h"

namespace {

std::string read_stdin() {
    std::ostringstream input;
    input << std::cin.rdbuf();
    return input.str();
}

void write_json_string(std::ostream& out, std::string_view value) {
    constexpr char hex[] = "0123456789abcdef";

    out.put('"');

    for (unsigned char c : value) {
        switch (c) {
            case '"':  out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b";  break;
            case '\f': out << "\\f";  break;
            case '\n': out << "\\n";  break;
            case '\r': out << "\\r";  break;
            case '\t': out << "\\t";  break;

            default:
                if (c < 0x20) {
                    out << "\\u00" << hex[c >> 4] << hex[c & 0x0f];
                } else {
                    out.put(static_cast<char>(c));
                }
                break;
        }
    }

    out.put('"');
}

void print_usage() {
    std::cerr
        << "Usage: qud [file.qu] [--target <target>]\n"
        << "Reads Quant source from stdin and writes diagnostics JSON to stdout.\n";
}

} // namespace

int main(int argc, char** argv) {
    std::string file;
    std::string target_name;
    bool target_specified = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];

        if (arg == "--target") {
            if (++i >= argc) {
                std::cerr << "error: --target requires a value\n";
                return 2;
            }

            target_name = argv[i];
            target_specified = true;
        } else if (arg == "-h" || arg == "--help") {
            print_usage();
            return 0;
        } else if (!arg.empty() && arg.front() == '-') {
            std::cerr << "error: unknown option: " << arg << '\n';
            return 2;
        } else if (file.empty()) {
            file = arg;
        } else {
            std::cerr << "error: unexpected argument: " << arg << '\n';
            return 2;
        }
    }

    const quant::codegen::Target* target = nullptr;

    if (target_specified) {
        const auto* info = quant::codegen::mc::resolve_target(target_name);

        if (!info || !info->enabled()) {
            std::cerr << "error: unknown or disabled target: "
                      << target_name << '\n';
            return 2;
        }

        target = info->target;
    }

    quant::driver::PipelineConfig cfg;
    cfg.input_source = read_stdin();
    cfg.input_name = file.empty() ? "<stdin>" : file;
    cfg.input_path = file;
    cfg.target = target;
    cfg.load_format_runtime = true;
    cfg.validate_linker = false;
    cfg.quiet_errors = true;

    quant::driver::Pipeline pipeline(cfg);
    pipeline.load();
    pipeline.analyze();

    const auto& errors = pipeline.ctx().errors;
    const auto& diagnostics = errors.all();
    const bool ok = !errors.has_errors();

    std::cout << "{\n"
              << "  \"status\": \"" << (ok ? "ok" : "error") << "\",\n"
              << "  \"errors\": [";

    if (!diagnostics.empty())
        std::cout << '\n';

    for (std::size_t i = 0; i < diagnostics.size(); ++i) {
        const auto& diagnostic = diagnostics[i];

        if (i != 0)
            std::cout << ",\n";

        std::cout << "    {\n" << "      \"file\": ";
        write_json_string(std::cout, diagnostic.file);

        std::cout << ",\n" << "      \"line\": " << diagnostic.line
                  << ",\n" << "      \"column\": " << diagnostic.column
                  << ",\n" << "      \"length\": " << diagnostic.length
                  << ",\n" << "      \"message\": ";
        write_json_string(std::cout, diagnostic.message);

        std::cout << "\n    }";
    }

    if (!diagnostics.empty())
        std::cout << '\n';

    std::cout << "  ]\n}\n";

    return ok ? 0 : 1;
}

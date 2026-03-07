// error_parser.hpp — Smart error parser for Buildm-on
// Parses output from rustc/cargo, gcc/clang, tsc/eslint, bazel
#pragma once

#include <string>
#include <vector>
#include <regex>
#include <algorithm>
#include <map>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// ─────────────────────────────────────────────
// Data Structures
// ─────────────────────────────────────────────

enum class ErrorSeverity {
    Error,
    Warning,
    Hint,
    Note
};

struct ParsedError {
    std::string file;
    int line = 0;
    int column = 0;
    std::string code;       // e.g. "E0308", "TS2304"
    std::string message;
    std::string context;    // surrounding source snippet
    ErrorSeverity severity = ErrorSeverity::Error;
    std::string tool;       // "rustc", "gcc", "tsc", etc.
};

inline std::string severity_str(ErrorSeverity s) {
    switch (s) {
        case ErrorSeverity::Error:   return "error";
        case ErrorSeverity::Warning: return "warning";
        case ErrorSeverity::Hint:    return "hint";
        case ErrorSeverity::Note:    return "note";
    }
    return "error";
}

inline void to_json(json& j, const ParsedError& e) {
    j = json{
        {"file", e.file}, {"line", e.line}, {"column", e.column},
        {"code", e.code}, {"message", e.message}, {"context", e.context},
        {"severity", severity_str(e.severity)}, {"tool", e.tool}
    };
}

inline void from_json(const json& j, ParsedError& e) {
    j.at("file").get_to(e.file);
    j.at("line").get_to(e.line);
    j.at("column").get_to(e.column);
    j.at("code").get_to(e.code);
    j.at("message").get_to(e.message);
    j.at("context").get_to(e.context);
    std::string sev = j.value("severity", "error");
    if (sev == "warning") e.severity = ErrorSeverity::Warning;
    else if (sev == "hint") e.severity = ErrorSeverity::Hint;
    else if (sev == "note") e.severity = ErrorSeverity::Note;
    else e.severity = ErrorSeverity::Error;
    j.at("tool").get_to(e.tool);
}

// ─────────────────────────────────────────────
// Parser Implementation
// ─────────────────────────────────────────────

class ErrorParser {
public:
    // Parse a block of build output lines
    std::vector<ParsedError> parse(const std::vector<std::string>& lines, const std::string& tool_hint = "") {
        std::vector<ParsedError> errors;

        for (size_t i = 0; i < lines.size(); i++) {
            const std::string& line = lines[i];
            ParsedError err;
            bool found = false;

            // ── Rust / cargo ──────────────────────────────────────
            // Format: error[E0308]: mismatched types
            //           --> src/main.rs:42:5
            if (!found) {
                static const std::regex rust_header(
                    R"(^(error|warning|note|help)(\[([A-Z][0-9]+)\])?: (.+)$)"
                );
                std::smatch m;
                if (std::regex_match(line, m, rust_header)) {
                    err.severity = sev_from_str(m[1]);
                    err.code = m[3];
                    err.message = m[4];
                    err.tool = "rustc";
                    // Look for the  --> file:line:col  on next lines
                    for (size_t k = i + 1; k < std::min(i + 4, lines.size()); k++) {
                        static const std::regex rust_loc(R"(^\s+-->\s+(.+):(\d+):(\d+))");
                        std::smatch lm;
                        if (std::regex_search(lines[k], lm, rust_loc)) {
                            err.file   = lm[1];
                            err.line   = std::stoi(lm[2]);
                            err.column = std::stoi(lm[3]);
                            break;
                        }
                    }
                    found = true;
                }
            }

            // ── cargo JSON (--message-format=json) ────────────────
            // {"reason":"compiler-message","message":{"level":"error","code":...}}
            if (!found && line.size() > 1 && line[0] == '{') {
                try {
                    auto j = json::parse(line);
                    if (j.value("reason", "") == "compiler-message") {
                        auto& msg = j["message"];
                        err.tool    = "cargo";
                        err.message = msg.value("message", "");
                        err.severity = sev_from_str(msg.value("level", "error"));
                        if (msg.contains("code") && !msg["code"].is_null())
                            err.code = msg["code"].value("code", "");
                        if (msg.contains("spans") && !msg["spans"].empty()) {
                            auto& span = msg["spans"][0];
                            err.file   = span.value("file_name", "");
                            err.line   = span.value("line_start", 0);
                            err.column = span.value("column_start", 0);
                        }
                        found = true;
                    }
                } catch (...) {}
            }

            // ── GCC / Clang ────────────────────────────────────────
            // Format: src/foo.c:42:7: error: implicit declaration of function 'bar'
            if (!found) {
                static const std::regex gcc_re(
                    R"(^([^:]+):(\d+):(\d+): (error|warning|note|fatal error): (.+)$)"
                );
                std::smatch m;
                if (std::regex_match(line, m, gcc_re)) {
                    err.file    = m[1];
                    err.line    = std::stoi(m[2]);
                    err.column  = std::stoi(m[3]);
                    err.severity = sev_from_str(m[4]);
                    err.message = m[5];
                    err.tool    = tool_hint.empty() ? "gcc" : tool_hint;
                    found = true;
                }
            }

            // ── TypeScript / tsc ───────────────────────────────────
            // Format: src/app.ts(42,7): error TS2304: Cannot find name 'foo'.
            if (!found) {
                static const std::regex ts_re(
                    R"(^(.+\.tsx?)\((\d+),(\d+)\): (error|warning) (TS\d+): (.+)$)"
                );
                std::smatch m;
                if (std::regex_match(line, m, ts_re)) {
                    err.file    = m[1];
                    err.line    = std::stoi(m[2]);
                    err.column  = std::stoi(m[3]);
                    err.severity = sev_from_str(m[4]);
                    err.code    = m[5];
                    err.message = m[6];
                    err.tool    = "tsc";
                    found = true;
                }
            }

            // ── ESLint ─────────────────────────────────────────────
            // Format:   42:7  error  'foo' is not defined  no-undef
            // (after a file header line like "/path/to/file.js")
            if (!found) {
                static const std::regex eslint_re(
                    R"(^\s+(\d+):(\d+)\s+(error|warning)\s+(.+?)\s{2,}([a-z/-]+)$)"
                );
                std::smatch m;
                if (std::regex_match(line, m, eslint_re)) {
                    err.line    = std::stoi(m[1]);
                    err.column  = std::stoi(m[2]);
                    err.severity = sev_from_str(m[3]);
                    err.message = m[4];
                    err.code    = m[5]; // rule name
                    err.tool    = "eslint";
                    // Look backwards for file path
                    for (int k = (int)i - 1; k >= 0; k--) {
                        if (!lines[k].empty() && lines[k][0] != ' ') {
                            err.file = lines[k];
                            break;
                        }
                    }
                    found = true;
                }
            }

            // ── Bazel ──────────────────────────────────────────────
            // Format: ERROR: path/to/BUILD:42:7: no such package 'foo'
            if (!found) {
                static const std::regex bazel_re(
                    R"(^(ERROR|WARNING): (.+):(\d+):(\d+): (.+)$)"
                );
                std::smatch m;
                if (std::regex_match(line, m, bazel_re)) {
                    err.severity = sev_from_str(m[1]);
                    err.file    = m[2];
                    err.line    = std::stoi(m[3]);
                    err.column  = std::stoi(m[4]);
                    err.message = m[5];
                    err.tool    = "bazel";
                    found = true;
                }
            }

            if (found && !err.message.empty()) {
                errors.push_back(std::move(err));
            }
        }

        return errors;
    }

private:
    static ErrorSeverity sev_from_str(const std::string& s) {
        std::string lo = s;
        std::transform(lo.begin(), lo.end(), lo.begin(), ::tolower);
        if (lo == "warning") return ErrorSeverity::Warning;
        if (lo == "hint" || lo == "help") return ErrorSeverity::Hint;
        if (lo == "note") return ErrorSeverity::Note;
        return ErrorSeverity::Error;
    }
};

// ─────────────────────────────────────────────
// Error Diff Engine
// ─────────────────────────────────────────────

struct ErrorDiff {
    std::vector<ParsedError> new_errors;       // appeared in new build
    std::vector<ParsedError> resolved_errors;  // present in old, gone in new
    std::vector<ParsedError> persisting;       // unchanged
};

inline std::string error_key(const ParsedError& e) {
    return e.file + ":" + std::to_string(e.line) + ":" + e.message;
}

inline ErrorDiff diff_errors(
    const std::vector<ParsedError>& old_errs,
    const std::vector<ParsedError>& new_errs)
{
    ErrorDiff result;
    std::map<std::string, bool> old_keys, new_keys;
    for (auto& e : old_errs) old_keys[error_key(e)] = true;
    for (auto& e : new_errs) new_keys[error_key(e)] = true;

    for (auto& e : new_errs) {
        if (old_keys.count(error_key(e))) result.persisting.push_back(e);
        else                               result.new_errors.push_back(e);
    }
    for (auto& e : old_errs) {
        if (!new_keys.count(error_key(e))) result.resolved_errors.push_back(e);
    }
    return result;
}

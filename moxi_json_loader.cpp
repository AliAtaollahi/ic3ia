#include "moxi_json_loader.h"

#include <cctype>
#include <cerrno>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace ic3ia {

namespace {

// -----------------------------
// Minimal JSON (enough for MoXI-JSON)
// -----------------------------
struct JVal {
    enum Kind { K_NULL, K_BOOL, K_NUM, K_STR, K_ARR, K_OBJ } k = K_NULL;

    bool b = false;
    double num = 0.0;
    std::string s;
    std::vector<JVal> a;
    std::map<std::string, JVal> o;

    static JVal make_null() { return JVal(); }
    static JVal make_bool(bool v) { JVal x; x.k = K_BOOL; x.b = v; return x; }
    static JVal make_num(double v) { JVal x; x.k = K_NUM; x.num = v; return x; }
    static JVal make_str(std::string v) { JVal x; x.k = K_STR; x.s = std::move(v); return x; }
    static JVal make_arr(std::vector<JVal> v) { JVal x; x.k = K_ARR; x.a = std::move(v); return x; }
    static JVal make_obj(std::map<std::string, JVal> v) { JVal x; x.k = K_OBJ; x.o = std::move(v); return x; }

    bool is_null() const { return k == K_NULL; }
    bool is_bool() const { return k == K_BOOL; }
    bool is_num()  const { return k == K_NUM; }
    bool is_str()  const { return k == K_STR; }
    bool is_arr()  const { return k == K_ARR; }
    bool is_obj()  const { return k == K_OBJ; }

    const std::string& as_str() const {
        if (!is_str()) throw std::runtime_error("JSON type error: expected string");
        return s;
    }
    const std::vector<JVal>& as_arr() const {
        if (!is_arr()) throw std::runtime_error("JSON type error: expected array");
        return a;
    }
    const std::map<std::string, JVal>& as_obj() const {
        if (!is_obj()) throw std::runtime_error("JSON type error: expected object");
        return o;
    }
    long long as_int() const {
        if (!is_num()) throw std::runtime_error("JSON type error: expected number");
        return static_cast<long long>(num);
    }

    bool has(const std::string &key) const {
        if (!is_obj()) return false;
        return o.find(key) != o.end();
    }
    const JVal& get(const std::string &key) const {
        if (!is_obj()) throw std::runtime_error("JSON type error: expected object for field access");
        auto it = o.find(key);
        if (it == o.end()) throw std::runtime_error("JSON missing field: " + key);
        return it->second;
    }
};

struct JsonParser {
    explicit JsonParser(const std::string &in) : s(in) {}

    JVal parse() {
        skip_ws();
        JVal v = parse_val();
        skip_ws();
        if (i != s.size()) throw std::runtime_error("JSON parse error: trailing characters");
        return v;
    }

private:
    const std::string &s;
    size_t i = 0;

    void skip_ws() {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    }

    char peek() const { return i < s.size() ? s[i] : '\0'; }
    char getc() { return i < s.size() ? s[i++] : '\0'; }

    void expect(char c) {
        if (getc() != c) {
            std::ostringstream oss;
            oss << "JSON parse error: expected '" << c << "'";
            throw std::runtime_error(oss.str());
        }
    }

    JVal parse_val() {
        skip_ws();
        char c = peek();
        if (c == '{') return parse_obj();
        if (c == '[') return parse_arr();
        if (c == '"') return JVal::make_str(parse_str());
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) return parse_num();
        if (starts_with("true")) { i += 4; return JVal::make_bool(true); }
        if (starts_with("false")) { i += 5; return JVal::make_bool(false); }
        if (starts_with("null")) { i += 4; return JVal::make_null(); }
        throw std::runtime_error("JSON parse error: invalid value");
    }

    bool starts_with(const char *lit) const {
        size_t n = 0;
        while (lit[n]) ++n;
        if (i + n > s.size()) return false;
        for (size_t k = 0; k < n; ++k) if (s[i+k] != lit[k]) return false;
        return true;
    }

    std::string parse_str() {
        expect('"');
        std::string out;
        while (i < s.size()) {
            char c = getc();
            if (c == '"') break;
            if (c == '\\') {
                if (i >= s.size()) throw std::runtime_error("JSON parse error: bad escape");
                char e = getc();
                switch (e) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        // Minimal \uXXXX handling (ASCII subset only)
                        if (i + 4 > s.size()) throw std::runtime_error("JSON parse error: bad \\u escape");
                        int code = 0;
                        for (int k = 0; k < 4; ++k) {
                            char h = s[i++];
                            code <<= 4;
                            if (h >= '0' && h <= '9') code |= (h - '0');
                            else if (h >= 'a' && h <= 'f') code |= (h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') code |= (h - 'A' + 10);
                            else throw std::runtime_error("JSON parse error: bad hex in \\u escape");
                        }
                        if (code >= 0 && code <= 0x7F) out.push_back(static_cast<char>(code));
                        else out.push_back('?'); // keep it simple
                        break;
                    }
                    default:
                        throw std::runtime_error("JSON parse error: unsupported escape");
                }
            } else {
                out.push_back(c);
            }
        }
        return out;
    }

    JVal parse_num() {
        size_t start = i;
        if (peek() == '-') ++i;
        while (std::isdigit(static_cast<unsigned char>(peek()))) ++i;
        if (peek() == '.') {
            ++i;
            while (std::isdigit(static_cast<unsigned char>(peek()))) ++i;
        }
        if (peek() == 'e' || peek() == 'E') {
            ++i;
            if (peek() == '+' || peek() == '-') ++i;
            while (std::isdigit(static_cast<unsigned char>(peek()))) ++i;
        }
        double v = std::stod(s.substr(start, i - start));
        return JVal::make_num(v);
    }

    JVal parse_arr() {
        expect('[');
        skip_ws();
        std::vector<JVal> out;
        if (peek() == ']') { getc(); return JVal::make_arr(std::move(out)); }
        while (true) {
            skip_ws();
            out.push_back(parse_val());
            skip_ws();
            char c = getc();
            if (c == ']') break;
            if (c != ',') throw std::runtime_error("JSON parse error: expected ',' or ']'");
        }
        return JVal::make_arr(std::move(out));
    }

    JVal parse_obj() {
        expect('{');
        skip_ws();
        std::map<std::string, JVal> out;
        if (peek() == '}') { getc(); return JVal::make_obj(std::move(out)); }
        while (true) {
            skip_ws();
            if (peek() != '"') throw std::runtime_error("JSON parse error: expected string key");
            std::string key = parse_str();
            skip_ws();
            expect(':');
            skip_ws();
            out.emplace(std::move(key), parse_val());
            skip_ws();
            char c = getc();
            if (c == '}') break;
            if (c != ',') throw std::runtime_error("JSON parse error: expected ',' or '}'");
        }
        return JVal::make_obj(std::move(out));
    }
};

static std::string read_all(const std::string &path) {
    std::ifstream in(path.c_str(), std::ios::in | std::ios::binary);
    if (!in) {
        std::ostringstream oss;
        oss << "Cannot open file: " << path;
        throw std::runtime_error(oss.str());
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// -----------------------------
// MoXI-JSON -> SMT-LIB2 (VMT annotated list)
// -----------------------------
struct VarInfo {
    std::string name;      // e.g., "a"
    std::string next_name; // e.g., "a.next"
    std::string sort;      // SMT-LIB2 sort string
};

static bool is_simple_smt_ident_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '$' || c == '-' || c == '@';
}
static std::string smt_ident(const std::string &raw) {
    // For typical MoXI variable names this returns raw unchanged.
    // If needed, quote with |...| for safety.
    if (raw.empty()) return "||";
    for (char c : raw) {
        if (!is_simple_smt_ident_char(c)) {
            std::string q = "|";
            for (char d : raw) {
                if (d == '|') q += "\\|";
                else q += d;
            }
            q += "|";
            return q;
        }
    }
    return raw;
}

static std::string sort_to_smt2(const JVal &sort_node);

static std::string sort_to_smt2(const JVal &sort_node) {
    // sort_node format: { "identifier": { "symbol": "...", "indices": [...] }, "parameters": [...] }
    const JVal &id = sort_node.get("identifier");
    std::string sym = id.get("symbol").as_str();
    const std::vector<JVal> *params = nullptr;
    if (sort_node.has("parameters")) params = &sort_node.get("parameters").as_arr();

    if (sym == "Bool" || sym == "Int" || sym == "Real") return sym;

    if (sym == "BitVec") {
        // SMT-LIB2: (_ BitVec N)
        const auto &idx = id.get("indices").as_arr();
        if (idx.empty()) throw std::runtime_error("BitVec sort missing size index");
        long long n = idx[0].as_int();
        std::ostringstream oss;
        oss << "(_ BitVec " << n << ")";
        return oss.str();
    }

    if (sym == "Array") {
        if (!params || params->size() != 2) throw std::runtime_error("Array sort expects 2 parameters");
        std::ostringstream oss;
        oss << "(Array " << sort_to_smt2((*params)[0]) << " " << sort_to_smt2((*params)[1]) << ")";
        return oss.str();
    }

    // Fallback: uninterpreted sort symbol
    return smt_ident(sym);
}

static const JVal& find_first_cmd(const std::vector<JVal> &cmds, const std::string &name) {
    for (const auto &c : cmds) {
        if (!c.is_obj()) continue;
        if (c.has("command") && c.get("command").as_str() == name) return c;
    }
    throw std::runtime_error("MoXI-JSON: command not found: " + name);
}

static const JVal& find_define_system(const std::vector<JVal> &cmds, const std::string &sym) {
    for (const auto &c : cmds) {
        if (!c.is_obj() || !c.has("command")) continue;
        if (c.get("command").as_str() == "define-system" && c.get("symbol").as_str() == sym) return c;
    }
    throw std::runtime_error("MoXI-JSON: define-system not found for symbol: " + sym);
}

static std::string term_to_smt2(const JVal &term,
                                const std::map<std::string, VarInfo> &vars,
                                bool prime_all,
                                std::map<std::string, std::string> let_bindings);

static std::string mk_app(const std::string &op, const std::vector<std::string> &args) {
    if (args.empty()) return "(" + op + ")";
    std::ostringstream oss;
    oss << "(" << op;
    for (const auto &a : args) oss << " " << a;
    oss << ")";
    return oss.str();
}

static std::string term_to_smt2(const JVal &term,
                                const std::map<std::string, VarInfo> &vars,
                                bool prime_all,
                                std::map<std::string, std::string> let_bindings)
{
    // term format:
    // { "identifier": <string|object>, "args": [ ... ] }
    const JVal &ident = term.get("identifier");

    // "let" is encoded inside identifier object, and body is in term.args[0]
    if (ident.is_obj() && ident.has("symbol") && ident.get("symbol").as_str() == "let") {
        const auto &binders = ident.get("binders").as_arr();
        std::ostringstream binds;
        binds << "(";
        bool first = true;
        for (const auto &b : binders) {
            std::string name = b.get("symbol").as_str();
            std::string rhs = term_to_smt2(b.get("term"), vars, prime_all, let_bindings);
            let_bindings[name] = rhs;

            if (!first) binds << " ";
            first = false;
            binds << "(" << smt_ident(name) << " " << rhs << ")";
        }
        binds << ")";

        const auto &args = term.get("args").as_arr();
        if (args.size() != 1) throw std::runtime_error("MoXI-JSON: let expects exactly 1 body term");
        std::string body = term_to_smt2(args[0], vars, prime_all, let_bindings);
        return "(let " + binds.str() + " " + body + ")";
    }

    // identifier is an object => operator / qualified identifier
    if (ident.is_obj()) {
        std::string sym = ident.get("symbol").as_str();

        std::vector<std::string> args_s;
        if (term.has("args")) {
            for (const auto &a : term.get("args").as_arr()) {
                args_s.push_back(term_to_smt2(a, vars, prime_all, let_bindings));
            }
        }

        // Special: array const with "as" qualifier
        // JSON typically: { "symbol":"const", "qualifier":"as", "sort":{...} } with args [value]
        if (sym == "const" && ident.has("qualifier") && ident.get("qualifier").as_str() == "as") {
            if (args_s.size() != 1) throw std::runtime_error("MoXI-JSON: (as const ...) expects 1 arg");
            // Prefer identifier.sort if present
            if (!ident.has("sort")) throw std::runtime_error("MoXI-JSON: (as const ...) missing sort");
            std::string as_sort = sort_to_smt2(ident.get("sort"));
            return "((as const " + as_sort + ") " + args_s[0] + ")";
        }

        // Indexed operators in SMT-LIB2
        auto mk_indexed1 = [&](const std::string &op) -> std::string {
            const auto &idx = ident.get("indices").as_arr();
            if (idx.size() != 1) throw std::runtime_error("MoXI-JSON: " + op + " expects 1 index");
            long long k = idx[0].as_int();
            if (args_s.size() != 1) throw std::runtime_error("MoXI-JSON: " + op + " expects 1 arg");
            std::ostringstream oss;
            oss << "((_ " << op << " " << k << ") " << args_s[0] << ")";
            return oss.str();
        };

        auto mk_indexed2 = [&](const std::string &op) -> std::string {
            const auto &idx = ident.get("indices").as_arr();
            if (idx.size() != 2) throw std::runtime_error("MoXI-JSON: " + op + " expects 2 indices");
            long long i0 = idx[0].as_int();
            long long i1 = idx[1].as_int();
            if (args_s.size() != 1) throw std::runtime_error("MoXI-JSON: " + op + " expects 1 arg");
            std::ostringstream oss;
            oss << "((_ " << op << " " << i0 << " " << i1 << ") " << args_s[0] << ")";
            return oss.str();
        };

        if (sym == "extract") return mk_indexed2("extract");
        if (sym == "zero_extend") return mk_indexed1("zero_extend");
        if (sym == "sign_extend") return mk_indexed1("sign_extend");
        if (sym == "rotate_left") return mk_indexed1("rotate_left");
        if (sym == "rotate_right") return mk_indexed1("rotate_right");

        // Generic operator application
        return mk_app(sym, args_s);
    }

    // identifier is a string => variable or literal
    if (!ident.is_str()) throw std::runtime_error("MoXI-JSON: identifier must be string or object");
    std::string id = ident.as_str();

    // let binding
    auto itb = let_bindings.find(id);
    if (itb != let_bindings.end()) return itb->second;

    // booleans
    if (id == "true" || id == "false") return id;

    // bitvector literals
    if (!id.empty() && id[0] == '#') return id;

    // primed var in MoXI: x'
    if (!id.empty() && id.back() == '\'') {
        std::string base = id.substr(0, id.size() - 1);
        auto it = vars.find(base);
        if (it == vars.end()) throw std::runtime_error("MoXI-JSON: unknown variable (primed): " + base);
        return smt_ident(it->second.next_name);
    }

    // numeric literal (MoXI JSON encodes numerals as strings)
    bool is_num = true;
    size_t p = 0;
    if (!id.empty() && (id[0] == '-' || id[0] == '+')) p = 1;
    if (p >= id.size()) is_num = false;
    for (; p < id.size(); ++p) {
        if (!std::isdigit(static_cast<unsigned char>(id[p]))) { is_num = false; break; }
    }
    if (is_num) return id;

    // plain variable
    auto it = vars.find(id);
    if (it != vars.end()) {
        if (prime_all) return smt_ident(it->second.next_name);
        return smt_ident(it->second.name);
    }

    // fallback: treat as a symbol constant
    return smt_ident(id);
}

static std::map<std::string, VarInfo> collect_vars(const JVal &def_sys_cmd,
                                                   bool include_inputs_as_state)
{
    std::map<std::string, VarInfo> out;

    auto add_list = [&](const char *field) {
        if (!def_sys_cmd.has(field)) return;
        const auto &lst = def_sys_cmd.get(field).as_arr();
        for (const auto &v : lst) {
            std::string name = v.get("symbol").as_str();
            std::string sort = sort_to_smt2(v.get("sort"));
            VarInfo info;
            info.name = name;
            info.next_name = name + ".next";
            info.sort = sort;
            out[name] = info;
        }
    };

    // MoXIchecker treats all variables uniformly; to mimic that,
    // include inputs too (even if empty).
    if (include_inputs_as_state) add_list("input");
    add_list("output");
    add_list("local");

    return out;
}

static std::string get_logic(const std::vector<JVal> &cmds) {
    for (const auto &c : cmds) {
        if (!c.is_obj() || !c.has("command")) continue;
        if (c.get("command").as_str() == "set-logic") return c.get("logic").as_str();
    }
    throw std::runtime_error("MoXI-JSON: missing set-logic");
}

static const JVal& get_check_system(const std::vector<JVal> &cmds) {
    return find_first_cmd(cmds, "check-system");
}

static std::string get_query_formula_symbol(const JVal &check_sys_cmd) {
    const auto &queries = check_sys_cmd.get("query").as_arr();
    if (queries.empty()) throw std::runtime_error("MoXI-JSON: check-system.query is empty");
    const auto &q0 = queries[0];
    const auto &forms = q0.get("formulas").as_arr();
    if (forms.empty()) throw std::runtime_error("MoXI-JSON: first query has no formulas");
    return forms[0].as_str();
}

static const JVal& find_reachable(const JVal &check_sys_cmd, const std::string &sym) {
    const auto &reach = check_sys_cmd.get("reachable").as_arr();
    for (const auto &r : reach) {
        if (r.get("symbol").as_str() == sym) return r;
    }
    throw std::runtime_error("MoXI-JSON: reachable formula not found: " + sym);
}

} // namespace

bool moxi_json_to_vmt_smt2(const std::string &json_path,
                           std::string &out_smt2,
                           std::string *err)
{
    try {
        std::string txt = read_all(json_path);
        JsonParser p(txt);
        JVal root = p.parse();

        if (!root.is_arr()) throw std::runtime_error("MoXI-JSON: top-level must be an array");
        const auto &cmds = root.as_arr();

        std::string logic = get_logic(cmds);
        const JVal &check_sys = get_check_system(cmds);
        std::string sys_sym = check_sys.get("symbol").as_str();
        const JVal &def_sys = find_define_system(cmds, sys_sym);

        // mimic MoXIchecker (uniform variable treatment)
        const bool include_inputs_as_state = true;
        std::map<std::string, VarInfo> vars = collect_vars(def_sys, include_inputs_as_state);

        // Extract MoXI formulas and print SMT-LIB2 expressions
        std::string init_s = term_to_smt2(def_sys.get("init"), vars, /*prime_all=*/false, {});
        std::string trans_s = term_to_smt2(def_sys.get("trans"), vars, /*prime_all=*/false, {});
        std::string inv_s = term_to_smt2(def_sys.get("inv"), vars, /*prime_all=*/false, {});
        std::string inv_next_s = term_to_smt2(def_sys.get("inv"), vars, /*prime_all=*/true, {});

        // Query: pick first referenced reachable formula (MoXIchecker behavior)
        std::string qsym = get_query_formula_symbol(check_sys);
        const JVal &reachable = find_reachable(check_sys, qsym);
        std::string query_s = term_to_smt2(reachable.get("formula"), vars, /*prime_all=*/false, {});
        std::string prop_s = "(not " + query_s + ")";

        // Compose (MoXIchecker semantics): init ∧ inv, trans ∧ inv'
        std::string init_final = "(and " + init_s + " " + inv_s + ")";
        std::string trans_final = "(and " + trans_s + " " + inv_next_s + ")";

        std::ostringstream out;
        out << "(set-logic " << logic << ")\n";

        // Declarations
        for (const auto &kv : vars) {
            const VarInfo &v = kv.second;
            out << "(declare-const " << smt_ident(v.name) << " " << v.sort << ")\n";
            out << "(declare-const " << smt_ident(v.next_name) << " " << v.sort << ")\n";
        }

        // :next annotations (VMT-style)
        for (const auto &kv : vars) {
            const VarInfo &v = kv.second;
            // define-fun wrapper to carry annotation on the term
            out << "(define-fun sv_" << smt_ident(v.name)
                << " () " << v.sort
                << " (! " << smt_ident(v.name)
                << " :next " << smt_ident(v.next_name)
                << "))\n";
        }

        // init/trans/property annotations
        out << "(define-fun init () Bool (! " << init_final << " :init true))\n";
        out << "(define-fun trans () Bool (! " << trans_final << " :trans true))\n";
        out << "(define-fun prop0 () Bool (! " << prop_s << " :invar-property 0))\n";

        // Allowed convenience terminator (as in ic3ia README/spec comment)
        out << "(assert true)\n";

        out_smt2 = out.str();
        return true;
    } catch (const std::exception &e) {
        if (err) *err = e.what();
        return false;
    }
}

} // namespace ic3ia

// Lab 7 — Query Parsing (Tokenizer + Recursive-Descent Parser + AST Executor)
// Takes a SQL SELECT ... FROM ... WHERE ... string, lexes it into tokens,
// builds an AST via recursive descent (so grammar encodes precedence), then
// walks the tree once per row to print the matching values.

#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include <stdexcept>
#include <cctype>

using namespace std;

// ─── Lexer ────────────────────────────────────────────────────────────────────

enum class TokKind {
    Select, From, Where, And, Or,
    Ident, Number,
    Gt, Lt, Eq, Gte, Lte,
    LParen, RParen,
    End
};

struct Token {
    TokKind kind;
    string  text;
};

static string toUpper(const string& s) {
    string r;
    r.reserve(s.size());
    for (char c : s)
        r.push_back((char)toupper((unsigned char)c));
    return r;
}

static vector<Token> lex(const string& src) {
    vector<Token> out;
    size_t i = 0;

    while (i < src.size()) {
        char c = src[i];

        if (isspace((unsigned char)c)) { ++i; continue; }

        // identifier or keyword
        if (isalpha((unsigned char)c) || c == '_') {
            size_t j = i;
            while (j < src.size() && (isalnum((unsigned char)src[j]) || src[j] == '_'))
                ++j;
            string word = src.substr(i, j - i);
            string up   = toUpper(word);
            i = j;

            if      (up == "SELECT") out.push_back({TokKind::Select, word});
            else if (up == "FROM")   out.push_back({TokKind::From,   word});
            else if (up == "WHERE")  out.push_back({TokKind::Where,  word});
            else if (up == "AND")    out.push_back({TokKind::And,    word});
            else if (up == "OR")     out.push_back({TokKind::Or,     word});
            else                     out.push_back({TokKind::Ident,  word});
            continue;
        }

        // number literal
        if (isdigit((unsigned char)c)) {
            size_t j = i;
            while (j < src.size() && isdigit((unsigned char)src[j])) ++j;
            out.push_back({TokKind::Number, src.substr(i, j - i)});
            i = j;
            continue;
        }

        // two-character operators: >= <=
        if ((c == '>' || c == '<') && i + 1 < src.size() && src[i + 1] == '=') {
            out.push_back({c == '>' ? TokKind::Gte : TokKind::Lte, string() + c + '='});
            i += 2;
            continue;
        }

        switch (c) {
            case '>': out.push_back({TokKind::Gt,     ">"});  ++i; break;
            case '<': out.push_back({TokKind::Lt,     "<"});  ++i; break;
            case '=': out.push_back({TokKind::Eq,     "="});  ++i; break;
            case '(': out.push_back({TokKind::LParen, "("}); ++i; break;
            case ')': out.push_back({TokKind::RParen, ")"}); ++i; break;
            default:
                throw runtime_error(string("unexpected character: ") + c);
        }
    }

    out.push_back({TokKind::End, ""});
    return out;
}

// ─── AST ──────────────────────────────────────────────────────────────────────
// One struct with a tag enum. Keeps the executor short and avoids
// dynamic_cast compared to a virtual class hierarchy.

enum class NodeKind { Column, Number, BinOp };

struct Expr {
    NodeKind           kind;
    string             text;      // column name, number text, or operator string
    unique_ptr<Expr>   lhs;
    unique_ptr<Expr>   rhs;
};

static unique_ptr<Expr> mkCol(const string& n) {
    return unique_ptr<Expr>(new Expr{NodeKind::Column, n, nullptr, nullptr});
}
static unique_ptr<Expr> mkNum(const string& n) {
    return unique_ptr<Expr>(new Expr{NodeKind::Number, n, nullptr, nullptr});
}
static unique_ptr<Expr> mkBin(const string& op,
                               unique_ptr<Expr> a,
                               unique_ptr<Expr> b) {
    return unique_ptr<Expr>(new Expr{NodeKind::BinOp, op, std::move(a), std::move(b)});
}


struct SelectStmt {
    string           col;
    string           table;
    unique_ptr<Expr> where;
};

class Parser {
public:
    explicit Parser(vector<Token> toks) : tokens(std::move(toks)) {}

    SelectStmt parse() {
        consume(TokKind::Select);
        string col   = consume(TokKind::Ident).text;
        consume(TokKind::From);
        string table = consume(TokKind::Ident).text;
        consume(TokKind::Where);
        auto   where = parseExpr();
        consume(TokKind::End);
        return SelectStmt{col, table, std::move(where)};
    }

private:
    // expr := term ( OR term )*
    unique_ptr<Expr> parseExpr() {
        auto left = parseTerm();
        while (tokens[pos].kind == TokKind::Or) {
            ++pos;
            auto right = parseTerm();
            left = mkBin("OR", std::move(left), std::move(right));
        }
        return left;
    }

    // term := factor ( AND factor )*
    unique_ptr<Expr> parseTerm() {
        auto left = parseFactor();
        while (tokens[pos].kind == TokKind::And) {
            ++pos;
            auto right = parseFactor();
            left = mkBin("AND", std::move(left), std::move(right));
        }
        return left;
    }

    // factor := '(' expr ')' | cmp
    unique_ptr<Expr> parseFactor() {
        if (tokens[pos].kind == TokKind::LParen) {
            ++pos;
            auto e = parseExpr();
            consume(TokKind::RParen);
            return e;
        }
        return parseCmp();
    }

    // cmp := IDENT op NUMBER
    unique_ptr<Expr> parseCmp() {
        string col = consume(TokKind::Ident).text;
        string op;
        switch (tokens[pos].kind) {
            case TokKind::Gt:  op = ">";  break;
            case TokKind::Lt:  op = "<";  break;
            case TokKind::Gte: op = ">="; break;
            case TokKind::Lte: op = "<="; break;
            case TokKind::Eq:  op = "=";  break;
            default:
                throw runtime_error("expected a comparison operator after: " + col);
        }
        ++pos;
        string num = consume(TokKind::Number).text;
        return mkBin(op, mkCol(col), mkNum(num));
    }

    Token consume(TokKind expected) {
        if (tokens[pos].kind != expected)
            throw runtime_error("unexpected token: \"" + tokens[pos].text + "\"");
        return tokens[pos++];
    }

    vector<Token> tokens;
    size_t        pos = 0;
};

// ─── Executor ─────────────────────────────────────────────────────────────────

struct Student {
    int    id;
    string name;
    int    marks;
};

static int colVal(const string& col, const Student& s) {
    if (col == "id")    return s.id;
    if (col == "marks") return s.marks;
    throw runtime_error("unknown column: " + col);
}

// recursive tree walk — one call per node
static bool matches(const Expr* e, const Student& row) {
    if (e->text == "AND") return matches(e->lhs.get(), row) && matches(e->rhs.get(), row);
    if (e->text == "OR")  return matches(e->lhs.get(), row) || matches(e->rhs.get(), row);

    // comparison node: lhs is a ColumnRef, rhs is a Number
    int lv = colVal(e->lhs->text, row);
    int rv = stoi(e->rhs->text);

    if (e->text == ">")  return lv >  rv;
    if (e->text == "<")  return lv <  rv;
    if (e->text == ">=") return lv >= rv;
    if (e->text == "<=") return lv <= rv;
    if (e->text == "=")  return lv == rv;

    throw runtime_error("unknown operator: " + e->text);
}

static void run(const SelectStmt& q, const vector<Student>& rows) {
    cout << "Result of: SELECT " << q.col
         << " FROM " << q.table << " WHERE ...\n";

    for (const Student& row : rows) {
        if (!matches(q.where.get(), row)) continue;
        if      (q.col == "name")  cout << "  " << row.name  << "\n";
        else if (q.col == "id")    cout << "  " << row.id    << "\n";
        else if (q.col == "marks") cout << "  " << row.marks << "\n";
    }
}

// ─── main ─────────────────────────────────────────────────────────────────────

int main() {
    vector<Student> students = {
        {1, "Riya", 92},
        {2, "Aarav",   75},
        {3, "Diya",    88},
        {4, "Kabir",   55},
        {5, "Meera",   81},
        {6, "Rohan",   47},
    };

    string sql = "SELECT name FROM students WHERE marks >= 80 AND id < 6";

    cout << "Query: " << sql << "\n\n";

    Parser     parser(lex(sql));
    SelectStmt stmt = parser.parse();
    run(stmt, students);

    return 0;
}

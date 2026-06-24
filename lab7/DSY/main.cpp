
#include <iostream>
#include <string>
#include <vector>
#include <stack>
#include <cctype>

using namespace std;

// a token is either an operand (column name / number) or an operator
struct Token {
    string val;
    bool   isOp;
};

// precedence: comparisons bind tightest, then AND, then OR
static int prec(const string& op) {
    if (op == ">" || op == "<" || op == ">=" || op == "<=" || op == "=")
        return 3;
    if (op == "AND") return 2;
    if (op == "OR")  return 1;
    return 0;
}

static string toUpper(const string& s) {
    string r;
    for (char c : s)
        r.push_back((char)toupper((unsigned char)c));
    return r;
}

// one-pass tokenizer — returns a flat vector<Token>
static vector<Token> tokenize(const string& src) {
    vector<Token> out;
    size_t i = 0;
    while (i < src.size()) {
        char c = src[i];

        if (isspace((unsigned char)c)) { ++i; continue; }

        // word: keyword (AND/OR) or column name
        if (isalpha((unsigned char)c) || c == '_') {
            size_t j = i;
            while (j < src.size() && (isalnum((unsigned char)src[j]) || src[j] == '_'))
                ++j;
            string word = src.substr(i, j - i);
            string up   = toUpper(word);
            if (up == "AND" || up == "OR")
                out.push_back({up, true});
            else
                out.push_back({word, false});
            i = j;
            continue;
        }

        // number literal
        if (isdigit((unsigned char)c)) {
            size_t j = i;
            while (j < src.size() && isdigit((unsigned char)src[j])) ++j;
            out.push_back({src.substr(i, j - i), false});
            i = j;
            continue;
        }

        // two-character operators: >= <=
        if ((c == '>' || c == '<') && i + 1 < src.size() && src[i + 1] == '=') {
            out.push_back({string() + c + '=', true});
            i += 2;
            continue;
        }

        // single-character operators and parens
        if (c == '>' || c == '<' || c == '=') {
            out.push_back({string(1, c), true});  ++i; continue;
        }
        if (c == '(' || c == ')') {
            out.push_back({string(1, c), false}); ++i; continue;
        }

        ++i; // skip anything else
    }
    return out;
}

// Dijkstra's Shunting-Yard — all operators are left-associative so we
// pop while top has precedence >= incoming operator's precedence
static vector<Token> shuntingYard(const vector<Token>& in) {
    vector<Token> out;
    stack<Token>  ops;

    for (const Token& t : in) {
        if (t.val == "(") {
            ops.push(t);
        } else if (t.val == ")") {
            while (!ops.empty() && ops.top().val != "(") {
                out.push_back(ops.top());
                ops.pop();
            }
            if (!ops.empty()) ops.pop(); // discard the '('
        } else if (t.isOp) {
            while (!ops.empty() &&
                   ops.top().val != "(" &&
                   prec(ops.top().val) >= prec(t.val)) {
                out.push_back(ops.top());
                ops.pop();
            }
            ops.push(t);
        } else {
            // operand goes straight to output
            out.push_back(t);
        }
    }

    while (!ops.empty()) {
        out.push_back(ops.top());
        ops.pop();
    }
    return out;
}

struct Student {
    string name;
    int    id;
    int    marks;
};

static bool isNumber(const string& s) {
    if (s.empty()) return false;
    for (char c : s)
        if (!isdigit((unsigned char)c)) return false;
    return true;
}

static int colVal(const string& col, const Student& s) {
    if (col == "id")    return s.id;
    if (col == "marks") return s.marks;
    return 0;
}

// walk the postfix once — numbers/columns push, operators pop two and push result
static bool evalPostfix(const vector<Token>& rpn, const Student& row) {
    stack<int> st;
    for (const Token& t : rpn) {
        if (!t.isOp) {
            st.push(isNumber(t.val) ? stoi(t.val) : colVal(t.val, row));
            continue;
        }
        int b = st.top(); st.pop();
        int a = st.top(); st.pop();
        if      (t.val == ">")   st.push(a >  b ? 1 : 0);
        else if (t.val == "<")   st.push(a <  b ? 1 : 0);
        else if (t.val == ">=")  st.push(a >= b ? 1 : 0);
        else if (t.val == "<=")  st.push(a <= b ? 1 : 0);
        else if (t.val == "=")   st.push(a == b ? 1 : 0);
        else if (t.val == "AND") st.push(a && b ? 1 : 0);
        else if (t.val == "OR")  st.push(a || b ? 1 : 0);
    }
    return st.top() != 0;
}

int main() {
    // same dataset as queryParsing/ so the two approaches are directly comparable
    vector<Student> students = {
        {"Riya", 1, 92},
        {"Aarav",   2, 75},
        {"Diya",    3, 88},
        {"Kabir",   4, 55},
        {"Meera",   5, 81},
        {"Rohan",   6, 47},
    };

    string clause = "marks >= 80 AND (id < 3 OR id > 4)";

    cout << "Infix WHERE : " << clause << "\n";

    auto toks = tokenize(clause);
    auto rpn  = shuntingYard(toks);

    cout << "Postfix     : ";
    for (const Token& t : rpn) cout << t.val << " ";
    cout << "\n\n";

    cout << "Rows matching WHERE clause:\n";
    for (const Student& s : students) {
        if (evalPostfix(rpn, s))
            cout << "  " << s.name
                 << " (id=" << s.id << ", marks=" << s.marks << ")\n";
    }

    return 0;
}

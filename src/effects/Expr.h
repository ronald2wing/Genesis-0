// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

// A small expression language a Pack's manifest may use to derive a uniform
// from its parameter values, e.g. `radius * 2.0`, `clamp(strength / 100.0,
// 0.0, 1.0)`, `max(a, b)`. All arithmetic is double; the grammar is:
//
//     expr    := add
//     add     := mul (('+' | '-') mul)*
//     mul     := unary (('*' | '/') unary)*
//     unary   := ('-' | '+') unary | primary
//     primary := number | name | '(' add ')' | call
//     call    := name '(' add (',' add)* ')'
//
// A number requires a leading digit: `digits [ '.' [digits] ] [ ('e'|'E')
// ['+'|'-'] digits ]`. The callable vocabulary is min(2), max(2), clamp(3),
// abs(1), floor(1), ceil(1) and round(1). Names are any identifier the caller
// knows; `check` is handed a predicate, `eval` a lookup.
//
// `check` validates structure only - syntax, the function vocabulary and
// arity, and that every name is known - without evaluating, so `1.0 / amount`
// is accepted even when amount is 0. `eval` evaluates and reports division by
// zero and non-finite results. Parsing is bounded at kMaxNodes nodes so a
// hostile manifest cannot recurse without limit.

namespace genesis::effects::expr {

// A compiled expression: the source parsed once into a flat postfix program,
// so the repeated eval a frame costs walks instructions instead of re-parsing
// text. Immutable once compiled; `compile` produces it and `eval(Compiled)`
// runs it against a lookup. The instruction set mirrors the parser's grammar
// exactly, so a compiled eval reports the same errors as `eval(source)`.
struct Compiled
{
    enum class Op : std::uint8_t {
        Push, // push the constant in `value`
        Name, // push the variable `names[index]`
        Add,
        Sub,
        Mul,
        Div,
        Neg, // negate the top of the stack
        Call, // apply the function `index` (a Fn) to its arity of values
    };

    // The built-in functions, by index into `Call` instructions.
    enum class Fn : std::uint32_t {
        Min,
        Max,
        Clamp,
        Abs,
        Floor,
        Ceil,
        Round,
    };

    struct Instr
    {
        Op op = Op::Push;
        double value = 0.0; // Push: the constant
        std::uint32_t index = 0; // Name: names index; Call: a Fn
    };

    std::vector<std::string> names; // the variables, in first-use order
    std::vector<Instr> program;
};

namespace detail {

inline constexpr std::size_t kMaxNodes = 256;

inline bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

inline bool is_name_start(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

inline bool is_name_char(char c)
{
    return is_name_start(c) || is_digit(c);
}

// The argument count a callable takes, or 0 for a name that is not one.
inline std::size_t function_arity(std::string_view name)
{
    if (name == "min")
        return 2;
    if (name == "max")
        return 2;
    if (name == "clamp")
        return 3;
    if (name == "abs")
        return 1;
    if (name == "floor")
        return 1;
    if (name == "ceil")
        return 1;
    if (name == "round")
        return 1;
    return 0;
}

inline double apply_function(std::string_view name, const std::vector<double> &args)
{
    if (name == "min")
        return std::min(args[0], args[1]);
    if (name == "max")
        return std::max(args[0], args[1]);
    // min(max(x, lo), hi), not std::clamp, whose behaviour is undefined when
    // lo > hi: an expression may legitimately spell either order.
    if (name == "clamp")
        return std::min(std::max(args[0], args[1]), args[2]);
    if (name == "abs")
        return std::abs(args[0]);
    if (name == "floor")
        return std::floor(args[0]);
    if (name == "ceil")
        return std::ceil(args[0]);
    return std::round(args[0]); // round
}

// The function id for `name`, or nullopt for a name that is not callable.
inline std::optional<Compiled::Fn> function_id(std::string_view name)
{
    if (name == "min")
        return Compiled::Fn::Min;
    if (name == "max")
        return Compiled::Fn::Max;
    if (name == "clamp")
        return Compiled::Fn::Clamp;
    if (name == "abs")
        return Compiled::Fn::Abs;
    if (name == "floor")
        return Compiled::Fn::Floor;
    if (name == "ceil")
        return Compiled::Fn::Ceil;
    if (name == "round")
        return Compiled::Fn::Round;
    return std::nullopt;
}

// The arity of a compiled function, so the evaluator pops the right count.
inline std::size_t function_arity(Compiled::Fn fn)
{
    switch (fn) {
    case Compiled::Fn::Min:
    case Compiled::Fn::Max:
        return 2;
    case Compiled::Fn::Clamp:
        return 3;
    case Compiled::Fn::Abs:
    case Compiled::Fn::Floor:
    case Compiled::Fn::Ceil:
    case Compiled::Fn::Round:
        return 1;
    }
    return 0;
}

inline double apply_function(Compiled::Fn fn, const std::array<double, 3> &args)
{
    switch (fn) {
    case Compiled::Fn::Min:
        return std::min(args[0], args[1]);
    case Compiled::Fn::Max:
        return std::max(args[0], args[1]);
    // min(max(x, lo), hi), not std::clamp, whose behaviour is undefined
    // when lo > hi: an expression may legitimately spell either order.
    case Compiled::Fn::Clamp:
        return std::min(std::max(args[0], args[1]), args[2]);
    case Compiled::Fn::Abs:
        return std::abs(args[0]);
    case Compiled::Fn::Floor:
        return std::floor(args[0]);
    case Compiled::Fn::Ceil:
        return std::ceil(args[0]);
    case Compiled::Fn::Round:
        return std::round(args[0]);
    }
    return args[0];
}

class Lexer
{
public:
    enum class Kind {
        End,
        Number,
        Name,
        Plus,
        Minus,
        Star,
        Slash,
        LParen,
        RParen,
        Comma,
    };

    struct Token
    {
        Kind kind = Kind::End;
        double number = 0.0;
        std::string_view name;
    };

    explicit Lexer(std::string_view source) : source_(source) { }

    const std::string &error() const { return error_; }
    bool failed() const { return !error_.empty(); }

    Token next()
    {
        skip_whitespace();
        if (pos_ >= source_.size()) {
            return { };
        }
        const char c = source_[pos_];
        switch (c) {
        case '+':
            ++pos_;
            return { Kind::Plus, 0.0, { } };
        case '-':
            ++pos_;
            return { Kind::Minus, 0.0, { } };
        case '*':
            ++pos_;
            return { Kind::Star, 0.0, { } };
        case '/':
            ++pos_;
            return { Kind::Slash, 0.0, { } };
        case '(':
            ++pos_;
            return { Kind::LParen, 0.0, { } };
        case ')':
            ++pos_;
            return { Kind::RParen, 0.0, { } };
        case ',':
            ++pos_;
            return { Kind::Comma, 0.0, { } };
        default:
            break;
        }
        if (is_digit(c)) {
            return number();
        }
        if (is_name_start(c)) {
            return name();
        }
        error_ = std::string("unexpected character `") + c + "`";
        return { };
    }

private:
    void skip_whitespace()
    {
        while (pos_ < source_.size()) {
            const char c = source_[pos_];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
                break;
            }
            ++pos_;
        }
    }

    Token number()
    {
        const std::size_t start = pos_;
        while (pos_ < source_.size() && is_digit(source_[pos_])) {
            ++pos_;
        }
        if (pos_ < source_.size() && source_[pos_] == '.') {
            ++pos_;
            while (pos_ < source_.size() && is_digit(source_[pos_])) {
                ++pos_;
            }
        }
        if (pos_ < source_.size() && (source_[pos_] == 'e' || source_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < source_.size() && (source_[pos_] == '+' || source_[pos_] == '-')) {
                ++pos_;
            }
            const std::size_t exponent_start = pos_;
            while (pos_ < source_.size() && is_digit(source_[pos_])) {
                ++pos_;
            }
            if (pos_ == exponent_start) {
                error_ = "a number's exponent needs digits";
                return { };
            }
        }
        const std::string_view text = source_.substr(start, pos_ - start);
        double value = 0.0;
        const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
        if (result.ec != std::errc{ }) {
            error_ = "number is out of range";
            return { };
        }
        return { Kind::Number, value, { } };
    }

    Token name()
    {
        const std::size_t start = pos_;
        while (pos_ < source_.size() && is_name_char(source_[pos_])) {
            ++pos_;
        }
        return { Kind::Name, 0.0, source_.substr(start, pos_ - start) };
    }

    std::string_view source_;
    std::size_t pos_ = 0;
    std::string error_;
};

class Parser
{
public:
    // In check mode the parser asks `is_name` whether each variable is known
    // and never applies the arithmetic (so a would-be division by zero is a
    // structural non-issue). In eval mode it asks `lookup` for each variable's
    // value and reports division by zero and non-finite results.
    Parser(std::string_view source, bool check_mode, std::function<bool(std::string_view)> is_name,
           std::function<std::optional<double>(std::string_view)> lookup, std::string *error)
        : lexer_(source),
          token_(lexer_.next()),
          check_mode_(check_mode),
          is_name_(std::move(is_name)),
          lookup_(std::move(lookup)),
          error_(error)
    {
    }

    std::optional<double> parse()
    {
        if (lexer_.failed()) {
            *error_ = lexer_.error();
            return std::nullopt;
        }
        const std::optional<double> value = parse_add();
        if (!ok()) {
            return std::nullopt;
        }
        if (token_.kind != Lexer::Kind::End) {
            fail("unexpected trailing input");
            return std::nullopt;
        }
        return value;
    }

private:
    bool ok() const { return error_->empty(); }

    void fail(std::string message)
    {
        if (error_->empty()) {
            *error_ = std::move(message);
        }
    }

    void check_finite(double value)
    {
        if (!std::isfinite(value)) {
            fail("result is not a finite number");
        }
    }

    void advance()
    {
        token_ = lexer_.next();
        if (lexer_.failed() && error_->empty()) {
            *error_ = lexer_.error();
        }
    }

    double parse_add()
    {
        double value = parse_mul();
        while (ok() && (token_.kind == Lexer::Kind::Plus || token_.kind == Lexer::Kind::Minus)) {
            const bool plus = token_.kind == Lexer::Kind::Plus;
            advance();
            const double rhs = parse_mul();
            if (!ok()) {
                return 0.0;
            }
            if (check_mode_) {
                continue;
            }
            value = plus ? value + rhs : value - rhs;
            check_finite(value);
        }
        return value;
    }

    double parse_mul()
    {
        double value = parse_unary();
        while (ok() && (token_.kind == Lexer::Kind::Star || token_.kind == Lexer::Kind::Slash)) {
            const bool star = token_.kind == Lexer::Kind::Star;
            advance();
            const double rhs = parse_unary();
            if (!ok()) {
                return 0.0;
            }
            if (check_mode_) {
                continue;
            }
            if (star) {
                value = value * rhs;
            } else {
                if (rhs == 0.0) {
                    fail("division by zero");
                    return 0.0;
                }
                value = value / rhs;
            }
            check_finite(value);
        }
        return value;
    }

    double parse_unary()
    {
        if (token_.kind == Lexer::Kind::Minus || token_.kind == Lexer::Kind::Plus) {
            const bool minus = token_.kind == Lexer::Kind::Minus;
            advance();
            const double value = parse_unary();
            if (!ok()) {
                return 0.0;
            }
            return check_mode_ ? 0.0 : (minus ? -value : value);
        }
        return parse_primary();
    }

    double parse_primary()
    {
        ++nodes_;
        if (nodes_ > kMaxNodes) {
            fail("expression is too complex");
            return 0.0;
        }
        if (token_.kind == Lexer::Kind::Number) {
            const double value = token_.number;
            advance();
            return value;
        }
        if (token_.kind == Lexer::Kind::Name) {
            return parse_name();
        }
        if (token_.kind == Lexer::Kind::LParen) {
            advance();
            const double value = parse_add();
            if (!ok()) {
                return 0.0;
            }
            if (token_.kind != Lexer::Kind::RParen) {
                fail("expected `)`");
                return 0.0;
            }
            advance();
            return value;
        }
        fail("expected a number, a parameter or `(`");
        return 0.0;
    }

    double parse_name()
    {
        const std::string_view name = token_.name;
        advance();
        if (const std::size_t arity = function_arity(name)) {
            if (token_.kind != Lexer::Kind::LParen) {
                fail("`" + std::string(name) + "` is a function and needs `(`");
                return 0.0;
            }
            advance();
            std::vector<double> args;
            if (token_.kind != Lexer::Kind::RParen) {
                args.push_back(parse_add());
                while (ok() && token_.kind == Lexer::Kind::Comma) {
                    advance();
                    args.push_back(parse_add());
                }
            }
            if (!ok()) {
                return 0.0;
            }
            if (token_.kind != Lexer::Kind::RParen) {
                fail("expected `)`");
                return 0.0;
            }
            advance();
            if (args.size() != arity) {
                fail("`" + std::string(name) + "` takes " + std::to_string(arity)
                     + " arguments, got " + std::to_string(args.size()));
                return 0.0;
            }
            if (check_mode_) {
                return 0.0;
            }
            return apply_function(name, args);
        }
        if (check_mode_) {
            if (!is_name_(name)) {
                fail("unknown parameter `" + std::string(name) + "`");
                return 0.0;
            }
            return 0.0;
        }
        const std::optional<double> value = lookup_(name);
        if (!value.has_value()) {
            fail("unknown parameter `" + std::string(name) + "`");
            return 0.0;
        }
        return *value;
    }

    Lexer lexer_;
    Lexer::Token token_;
    bool check_mode_;
    std::function<bool(std::string_view)> is_name_;
    std::function<std::optional<double>(std::string_view)> lookup_;
    std::string *error_;
    std::size_t nodes_ = 0;
};

// Emits the postfix program for a source, mirroring `Parser`'s grammar exactly
// (same precedence, same function vocabulary and arity, same node bound) so a
// compiled eval agrees with `eval(source)` on every well-formed input and every
// error. Produces no values: names and functions are recorded, not applied.
class Compiler
{
public:
    Compiler(std::string_view source, std::string *error)
        : lexer_(source), token_(lexer_.next()), error_(error)
    {
    }

    std::optional<Compiled> compile()
    {
        if (lexer_.failed()) {
            *error_ = lexer_.error();
            return std::nullopt;
        }
        compile_add();
        if (!ok()) {
            return std::nullopt;
        }
        if (token_.kind != Lexer::Kind::End) {
            fail("unexpected trailing input");
            return std::nullopt;
        }
        Compiled out;
        out.names = std::move(names_);
        out.program = std::move(program_);
        return out;
    }

private:
    bool ok() const { return error_->empty(); }

    void fail(std::string message)
    {
        if (error_->empty()) {
            *error_ = std::move(message);
        }
    }

    void advance()
    {
        token_ = lexer_.next();
        if (lexer_.failed() && error_->empty()) {
            *error_ = lexer_.error();
        }
    }

    void emit(Compiled::Op op, double value, std::uint32_t index)
    {
        program_.push_back(Compiled::Instr{ op, value, index });
    }

    // The index of `name` in names_, interning it on first use.
    std::uint32_t intern(std::string_view name)
    {
        for (std::size_t i = 0; i < names_.size(); ++i) {
            if (names_[i] == name) {
                return static_cast<std::uint32_t>(i);
            }
        }
        names_.emplace_back(name);
        return static_cast<std::uint32_t>(names_.size() - 1);
    }

    void compile_add()
    {
        compile_mul();
        while (ok() && (token_.kind == Lexer::Kind::Plus || token_.kind == Lexer::Kind::Minus)) {
            const bool plus = token_.kind == Lexer::Kind::Plus;
            advance();
            compile_mul();
            if (!ok()) {
                return;
            }
            emit(plus ? Compiled::Op::Add : Compiled::Op::Sub, 0.0, 0);
        }
    }

    void compile_mul()
    {
        compile_unary();
        while (ok() && (token_.kind == Lexer::Kind::Star || token_.kind == Lexer::Kind::Slash)) {
            const bool star = token_.kind == Lexer::Kind::Star;
            advance();
            compile_unary();
            if (!ok()) {
                return;
            }
            emit(star ? Compiled::Op::Mul : Compiled::Op::Div, 0.0, 0);
        }
    }

    void compile_unary()
    {
        if (token_.kind == Lexer::Kind::Minus || token_.kind == Lexer::Kind::Plus) {
            const bool minus = token_.kind == Lexer::Kind::Minus;
            advance();
            compile_unary();
            if (!ok()) {
                return;
            }
            if (minus) {
                emit(Compiled::Op::Neg, 0.0, 0);
            }
            return;
        }
        compile_primary();
    }

    void compile_primary()
    {
        ++nodes_;
        if (nodes_ > kMaxNodes) {
            fail("expression is too complex");
            return;
        }
        if (token_.kind == Lexer::Kind::Number) {
            const double value = token_.number;
            advance();
            emit(Compiled::Op::Push, value, 0);
            return;
        }
        if (token_.kind == Lexer::Kind::Name) {
            const std::string_view name = token_.name;
            advance();
            if (const std::size_t arity = function_arity(name)) {
                if (token_.kind != Lexer::Kind::LParen) {
                    fail("`" + std::string(name) + "` is a function and needs `(`");
                    return;
                }
                advance();
                std::size_t argc = 0;
                if (token_.kind != Lexer::Kind::RParen) {
                    compile_add();
                    ++argc;
                    while (ok() && token_.kind == Lexer::Kind::Comma) {
                        advance();
                        compile_add();
                        ++argc;
                    }
                }
                if (!ok()) {
                    return;
                }
                if (token_.kind != Lexer::Kind::RParen) {
                    fail("expected `)`");
                    return;
                }
                advance();
                if (argc != arity) {
                    fail("`" + std::string(name) + "` takes " + std::to_string(arity)
                         + " arguments, got " + std::to_string(argc));
                    return;
                }
                emit(Compiled::Op::Call, 0.0, static_cast<std::uint32_t>(*function_id(name)));
                return;
            }
            emit(Compiled::Op::Name, 0.0, intern(name));
            return;
        }
        if (token_.kind == Lexer::Kind::LParen) {
            advance();
            compile_add();
            if (!ok()) {
                return;
            }
            if (token_.kind != Lexer::Kind::RParen) {
                fail("expected `)`");
                return;
            }
            advance();
            return;
        }
        fail("expected a number, a parameter or `(`");
    }

    Lexer lexer_;
    Lexer::Token token_;
    std::string *error_;
    std::size_t nodes_ = 0;
    std::vector<std::string> names_;
    std::vector<Compiled::Instr> program_;
};

} // namespace detail

// Whether `source` is a well-formed expression whose every variable is a name
// `is_name` accepts. Structure only - never evaluated, so a division by zero
// is accepted at load and reported later, at resolve, when the value exists.
inline bool check(std::string_view source, const std::function<bool(std::string_view)> &is_name,
                  std::string *error)
{
    std::string scratch;
    if (error == nullptr) {
        error = &scratch;
    }
    error->clear();
    detail::Parser parser(source, /*check_mode=*/true, is_name, { }, error);
    return parser.parse().has_value();
}

// The value of `source`, with every variable resolved through `lookup` (a
// nullopt is an unknown parameter). Returns nullopt - with `error` set - on a
// syntax error, an unknown parameter, a division by zero or a non-finite
// result.
inline std::optional<double>
eval(std::string_view source, const std::function<std::optional<double>(std::string_view)> &lookup,
     std::string *error)
{
    std::string scratch;
    if (error == nullptr) {
        error = &scratch;
    }
    error->clear();
    detail::Parser parser(source, /*check_mode=*/false, { }, lookup, error);
    return parser.parse();
}

// Compiles `source` into a reusable program. Returns nullopt - with `error`
// set - on a syntax error or an out-of-bounds arity, exactly the structural
// failures `check` reports. The compiled form never applies arithmetic, so a
// division by zero is a runtime concern for `eval(Compiled)`, not `compile`.
inline std::optional<Compiled> compile(std::string_view source, std::string *error)
{
    std::string scratch;
    if (error == nullptr) {
        error = &scratch;
    }
    error->clear();
    detail::Compiler compiler(source, error);
    return compiler.compile();
}

// The value of a compiled expression, with every variable resolved through
// `lookup`. Reports - via `error` - an unknown parameter, a division by zero
// or a non-finite result, in the same order `eval(source)` would, because the
// compiled program mirrors the parser's evaluation.
inline std::optional<double>
eval(const Compiled &compiled, const std::function<std::optional<double>(std::string_view)> &lookup,
     std::string *error)
{
    std::string scratch;
    if (error == nullptr) {
        error = &scratch;
    }
    error->clear();
    std::vector<double> stack;
    stack.reserve(compiled.program.size());
    for (const Compiled::Instr &in : compiled.program) {
        switch (in.op) {
        case Compiled::Op::Push:
            stack.push_back(in.value);
            break;
        case Compiled::Op::Name: {
            const std::optional<double> value = lookup(compiled.names[in.index]);
            if (!value.has_value()) {
                *error = "unknown parameter `" + compiled.names[in.index] + "`";
                return std::nullopt;
            }
            stack.push_back(*value);
            break;
        }
        case Compiled::Op::Neg:
            stack.back() = -stack.back();
            break;
        case Compiled::Op::Add:
        case Compiled::Op::Sub:
        case Compiled::Op::Mul:
        case Compiled::Op::Div: {
            const double rhs = stack.back();
            stack.pop_back();
            const double lhs = stack.back();
            stack.pop_back();
            double result = 0.0;
            switch (in.op) {
            case Compiled::Op::Add:
                result = lhs + rhs;
                break;
            case Compiled::Op::Sub:
                result = lhs - rhs;
                break;
            case Compiled::Op::Mul:
                result = lhs * rhs;
                break;
            case Compiled::Op::Div:
                if (rhs == 0.0) {
                    *error = "division by zero";
                    return std::nullopt;
                }
                result = lhs / rhs;
                break;
            default:
                break;
            }
            if (!std::isfinite(result)) {
                *error = "result is not a finite number";
                return std::nullopt;
            }
            stack.push_back(result);
            break;
        }
        case Compiled::Op::Call: {
            const Compiled::Fn fn = static_cast<Compiled::Fn>(in.index);
            const std::size_t arity = detail::function_arity(fn);
            std::array<double, 3> args{ 0.0, 0.0, 0.0 };
            for (std::size_t i = arity; i-- > 0;) {
                args[i] = stack.back();
                stack.pop_back();
            }
            stack.push_back(detail::apply_function(fn, args));
            break;
        }
        }
    }
    return stack.back();
}

} // namespace genesis::effects::expr

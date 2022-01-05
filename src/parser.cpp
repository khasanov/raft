#include "parser.h"

#include "raft.h"

namespace raft {

template <typename Base, typename T>
inline bool instanceof (const T *ptr)
{
    return dynamic_cast<const Base *>(ptr) != nullptr;
}

RuntimeError::RuntimeError(const Token &token, const std::string &message)
    : std::runtime_error{message}
    , token{token}
{
}

/*
A recursive descent parser is a literal translation of the grammar's rules straight into imperative
code. Each rule becomes a function. The body of the rule translates to code roughtly like:

Grammar notation  Code representation
Terminal          Code to match and consule a token
Nonterminal       Call to that rule's function
|                 if or switch statement
* or +            while of for loop
?                 if statement
*/
Parser::Parser(const std::vector<Token> &tokens)
    : tokens{tokens}
{
}

// program :: declaration* EOF
std::vector<Stmt *> Parser::parse()
{
    std::vector<Stmt *> statements;
    try {
        while (!isAtEnd()) {
            statements.emplace_back(declaration());
        }
    } catch (const RuntimeError &ex) {
        Raft::error(ex.token.line, ex.what());
        synchronize();
    }
    return statements;
}

// expression :: assignment
Expr *Parser::expression()
{
    return assignment();
}

// assignment :: IDENTIFIER "=" assignment | logic_or
Expr *Parser::assignment()
{
    Expr *expr = logicOr();
    if (match(Token::Type::Equal)) {
        Token equals = previous();
        Expr *value = assignment();

        if (instanceof <Variable>(expr)) {
            Token name = static_cast<Variable *>(expr)->name;
            return makeAstNode<Assign>(name, value);
        }
        Raft::error(equals.line, "Invalid assignment target");
    }
    return expr;
}

// equality :: comparison ( ( "!=" | "==" ) comparision )*
Expr *Parser::equality()
{
    Expr *expr = comparison();
    while (match(Token::Type::BangEqual, Token::Type::EqualEqual)) {
        Token op = previous();
        Expr *right = comparison();
        expr = makeAstNode<Binary>(expr, op, right);
    }
    return expr;
}

// comparison :: term ( ( ">" | ">=" | "<" | "<=" ) term )*
Expr *Parser::comparison()
{
    Expr *expr = term();
    while (match(Token::Type::GreaterThanSign, Token::Type::GreaterEqual, Token::Type::LessThanSign,
                 Token::Type::LessEqual)) {
        Token op = previous();
        Expr *right = term();
        expr = makeAstNode<Binary>(expr, op, right);
    }
    return expr;
}

// term :: factor ( ("-" | "+" ) factor )*
Expr *Parser::term()
{
    Expr *expr = factor();
    while (match(Token::Type::Minus, Token::Type::Plus)) {
        Token op = previous();
        Expr *right = factor();
        expr = makeAstNode<Binary>(expr, op, right);
    }
    return expr;
}

// factor :: unary ( ( "/" | "*" ) unary )*
Expr *Parser::factor()
{
    Expr *expr = unary();
    while (match(Token::Type::Slash, Token::Type::Star)) {
        Token op = previous();
        Expr *right = unary();
        expr = makeAstNode<Binary>(expr, op, right);
    }
    return expr;
}

// logic_or :: logic_and ( "or" logic_and )*
Expr *Parser::logicOr()
{
    Expr *expr = logicAnd();
    while (match(Token::Type::Or)) {
        Token op = previous();
        Expr *right = logicAnd();
        expr = makeAstNode<Logical>(expr, op, right);
    }
    return expr;
}

// logic_and :: equality ( "and" equality )*
Expr *Parser::logicAnd()
{
    Expr *expr = equality();
    while (match(Token::Type::And)) {
        Token op = previous();
        Expr *right = equality();
        expr = makeAstNode<Logical>(expr, op, right);
    }
    return expr;
}

// unary :: ( "!" "-" ) unary | primary
Expr *Parser::unary()
{
    if (match(Token::Type::Bang, Token::Type::Minus)) {
        Token op = previous();
        Expr *right = unary();
        return makeAstNode<Unary>(op, right);
    }
    return primary();
}

// primary :: NUMBER | STRING | "true" | "false" | "nil" | "(" expression ")" | IDENTIFIER
Expr *Parser::primary()
{
    if (match(Token::Type::False)) {
        return makeAstNode<Literal>(object::Boolean{false});
    }
    if (match(Token::Type::True)) {
        return makeAstNode<Literal>(object::Boolean{true});
    }
    if (match(Token::Type::Nil)) {
        return makeAstNode<Literal>(object::Null{});
    }
    if (match(Token::Type::NumberLiteral, Token::Type::StringLiteral)) {
        return makeAstNode<Literal>(previous().literal);
    }
    if (match(Token::Type::Identifier)) {
        return makeAstNode<Variable>(previous());
    }

    if (match(Token::Type::LeftParenthesis)) {
        Expr *expr = expression();
        consume(Token::Type::RightParenthesis, "Exprect ')' after expression");
        return makeAstNode<Grouping>(expr);
    }

    throw RuntimeError{peek(), "Expect expression"};
}

// declaration :: varDecl | statement
Stmt *Parser::declaration()
{
    try {
        if (match(Token::Type::Var)) {
            return varDeclaration();
        }
        return statement();
    } catch (const RuntimeError &ex) {
        Raft::error(ex.token.line, ex.what());
        synchronize();
        return nullptr;
    }
}

// "var" IDENTIFIER ( "=" expression )? ";"
Stmt *Parser::varDeclaration()
{
    Token name = consume(Token::Type::Identifier, "Expect variable name");
    Expr *initializer = nullptr;
    if (match(Token::Type::Equal)) {
        initializer = expression();
    }
    consume(Token::Type::Semicolon, "Expect ';' after variable declaration");
    return makeAstNode<VarDecl>(name, initializer);
}

// statement :: exprStmt | forStmt | ifStmt | printStmt | whileStmt | block
Stmt *Parser::statement()
{
    if (match(Token::Type::For)) {
        return forStatement();
    }
    if (match(Token::Type::If)) {
        return ifStatement();
    }
    if (match(Token::Type::Print)) {
        return printStatement();
    }
    if (match(Token::Type::While)) {
        return whileStatement();
    }
    if (match(Token::Type::LeftCurlyBracket)) {
        return makeAstNode<Block>(block());
    }
    return expressionStatement();
}

// forStmt :: "for" "(" (varDecl | exprStmt | ";" ) expression? ";" expression? ")" statement
Stmt *Parser::forStatement()
{
    consume(Token::Type::LeftParenthesis, "Expect '(' after 'for'");

    Stmt *initializer = nullptr;
    if (match(Token::Type::Semicolon)) {
        initializer = nullptr;
    } else if (match(Token::Type::Var)) {
        initializer = varDeclaration();
    } else {
        initializer = expressionStatement();
    }

    Expr *condition = nullptr;
    if (!check(Token::Type::Semicolon)) {
        condition = expression();
    }
    consume(Token::Type::Semicolon, "Expect ';' after loop condition");

    Expr *increment = nullptr;
    if (!check(Token::Type::RightParenthesis)) {
        increment = expression();
    }
    consume(Token::Type::RightParenthesis, "Expect ')' after for clauses");

    Stmt *body = statement();

    /* desugaring for statement to while statement
    for(<init>; <cond>; <iter>) <body> is equvivalent to

    <init>
    while(<cond>) {
      <body>
      <iter>
    }
    */

    if (increment != nullptr) {
        std::vector<Stmt *> stmts;
        stmts.emplace_back(body);
        stmts.emplace_back(makeAstNode<ExprStmt>(increment));
        body = makeAstNode<Block>(stmts);
    }
    if (condition == nullptr) {
        condition = makeAstNode<Literal>(object::Boolean{true});
    }
    body = makeAstNode<While>(condition, body);

    if (initializer) {
        std::vector<Stmt *> stmts;
        stmts.emplace_back(initializer);
        stmts.emplace_back(body);
        body = makeAstNode<Block>(std::move(stmts));
    }
    return body;
}

// ifStmt :: "if" "(" expression ")" statement ( "else" statement )?
Stmt *Parser::ifStatement()
{
    consume(Token::Type::LeftParenthesis, "Exprect '(' after 'if'");
    Expr *condition = expression();
    consume(Token::Type::RightParenthesis, "Exprect ')' after if condition");

    Stmt *thenBranch = statement();
    Stmt *elseBranch = nullptr;
    if (match(Token::Type::Else)) {
        elseBranch = statement();
    }
    return makeAstNode<If>(condition, thenBranch, elseBranch);
}

// printStmt :: "print" expression ";"
Stmt *Parser::printStatement()
{
    Expr *value = expression();
    consume(Token::Type::Semicolon, "Expect ';' after value");
    return makeAstNode<Print>(value);
}

// whileStmt   :: "while" "(" expression ")" statement;
Stmt *Parser::whileStatement()
{
    consume(Token::Type::LeftParenthesis, "Expect '(' after 'while'");
    Expr *condition = expression();
    consume(Token::Type::RightParenthesis, "Exprect ')' after condition");
    Stmt *body = statement();

    return makeAstNode<While>(condition, body);
}

// block :: "{" declaration* "}"
std::vector<Stmt *> Parser::block()
{
    std::vector<Stmt *> statements;
    while (!check(Token::Type::RightCurlyBracket) and !isAtEnd()) {
        statements.emplace_back(declaration());
    }
    consume(Token::Type::RightCurlyBracket, "Exprect '}' after block");
    return statements;
}

// exprStmt :: expresson ";"
Stmt *Parser::expressionStatement()
{
    Expr *expr = expression();
    consume(Token::Type::Semicolon, "Exprect ';' after expession");
    return makeAstNode<ExprStmt>(expr);
}

bool Parser::isAtEnd()
{
    return peek().type == Token::Type::EndOfFile;
}

Token Parser::peek()
{
    return tokens.at(current);
}

Token Parser::previous()
{
    return tokens.at(current - 1);
}

Token Parser::advance()
{
    if (!isAtEnd()) {
        current++;
    }
    return previous();
}

bool Parser::check(Token::Type type)
{
    if (isAtEnd()) {
        return false;
    }
    return peek().type == type;
}

Token Parser::consume(Token::Type type, const std::string &message)
{
    if (check(type)) {
        return advance();
    }
    throw RuntimeError{peek(), message};
}

void Parser::synchronize()
{
    advance();

    while (!isAtEnd()) {
        if (previous().type == Token::Type::Semicolon) {
            return;
        }

        switch (peek().type) {
        case Token::Type::Class:
            [[fallthrough]];
        case Token::Type::Fun:
            [[fallthrough]];
        case Token::Type::Var:
            [[fallthrough]];
        case Token::Type::For:
            [[fallthrough]];
        case Token::Type::If:
            [[fallthrough]];
        case Token::Type::While:
            [[fallthrough]];
        case Token::Type::Print:
            [[fallthrough]];
        case Token::Type::Return:
            return;
        default:
            break;
        }

        advance();
    }
}

}  // namespace raft

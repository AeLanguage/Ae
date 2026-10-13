#pragma once
// Ae 语法分析器：Token → AST
// 本文件是纯语法层：只依赖 Lexer，不碰常量池、符号表、字节码。
// 这是它能被字节码后端与 AOT 后端同时复用的根本原因。
//
// 相比"边解析边发射"的做法，这里有两点本质简化：
// ① 不再需要 declOnly 预扫描。老实现要靠第一遍登记函数签名来支持"调用后面
// 定义的函数"；有了 AST，前向引用天然成立（遍历时再解析符号即可）。
// ② 不再需要"边解析边 emit"。解析就是建树，代码生成完全是另一趟遍历。
#include <string>
#include <vector>
#include <deque>
#include <memory>
#include <stdexcept>
#include "ae_common.h"
#include "ae_lexer.h"
#include "ae_ast.h"

namespace aeparse {

using namespace aeast;

class Parser {
public:
    explicit Parser(const std::string& src, const std::string& path = "<source>",
                    uint16_t fileIdx = 0)
        : L(src), srcPath(path), fileIdx_(fileIdx) {
        L.fileIdx = fileIdx;
        advance();
    }

    // 解析整个编译单元
    Program parseProgram(const std::string& moduleName = std::string()) {
        Program p;
        p.sourcePath = srcPath;
        p.moduleName = moduleName;
        p.isModule   = !moduleName.empty();
        while (cur.type != TK_EOF) {
            StmtP s = topLevelStmt();
            if (s) p.stmts.push_back(std::move(s));
        }
        return p;
    }

    const std::string& path() const { return srcPath; }
    const std::vector<std::string>& imports() const { return impNames; }

private:
    Lexer  L;
    Token  cur;
    std::deque<Token> q;                 // 前瞻 / replay 缓冲
    std::string srcPath;
    uint16_t fileIdx_ = 0;
    std::vector<std::string> impNames;   // 遇到的 imp（交给编译期去真正加载）

    // 词法访问
    void advance() {
        if (!q.empty()) { cur = q.front(); q.pop_front(); }
        else            { cur = L.next(); }
    }
    Token peek(int n = 0) {
        while ((int)q.size() <= n) q.push_back(L.next());
        return q[n];
    }
    // 把一串 token 放回待消费位置（推导里的表达式要"先收集、后解析"）
    void replay(const std::vector<Token>& toks) {
        std::deque<Token> nq;
        for (const Token& t : toks) nq.push_back(t);
        while (!q.empty()) { nq.push_back(q.front()); q.pop_front(); }
        q = std::move(nq);
        advance();
    }

    // 诊断
    [[noreturn]] void err(const std::string& msg, int len = 1) const {
        throw std::runtime_error(aePack(fileIdx_, cur.line, cur.col, len, msg));
    }
    [[noreturn]] void errAt(const Token& t, const std::string& msg, int len = 1) const {
        throw std::runtime_error(aePack(fileIdx_, t.line, t.col, len, msg));
    }
    void expect(TK t, const std::string& what) {
        if (cur.type != t)
            err("语法错误: 期望 " + what + (cur.text.empty() ? "" : ("，得到 '" + cur.text + "'")));
        advance();
    }
    void mark(Expr* e) { if (e) { e->line = cur.line; e->col = cur.col; } }
    void markS(Stmt* s) { if (s) { s->line = cur.line; s->col = cur.col; } }

    // 顶层
    StmtP topLevelStmt() {
        if (cur.type == TK_FUNC) { auto f = funcDef(); return f; }
        if (cur.type == TK_IMP)  { return impStmt(); }
        return stmt();
    }

    StmtP impStmt() {
        auto s = mk<ImpStmt>(); markS(s.get());
        advance();                                  // imp
        if (cur.type == TK_IDENT) { s->modName = cur.text; advance(); }
        else err("imp 后需要库名/模块名");
        impNames.push_back(s->modName);
        return s;
    }

    // 函数定义
    StmtP funcDef() {
        Token kw = cur;
        advance();                                   // func
        auto f = mk<FuncDef>(); f->line = kw.line; f->col = kw.col;
        if (cur.type != TK_IDENT) errAt(kw, "func 后需要函数名");
        f->name = cur.text; advance();
        expect(TK_LPAREN, "(");
        parseParamList(f->params);
        if (f->name == "main" && !f->params.empty()) errAt(kw, "main 不能有参数");
        expect(TK_LBRACE, "{");
        f->body = blockBody();
        return f;
    }

    void parseParamList(std::vector<Param>& out) {
        out.clear(); sawDefault = false;
        if (cur.type == TK_RPAREN) { advance(); return; }
        for (;;) {
            if (cur.type != TK_IDENT) err("参数名需要标识符");
            Param p; p.name = cur.text; advance();
            if (cur.type == TK_ELLIPSIS) {           // 变参：必须在末尾
                p.variadic = true; advance();
                out.push_back(std::move(p));
                if (cur.type == TK_COMMA) err("变参 '...' 必须是最后一个参数");
                break;
            }
            if (cur.type == TK_ASSIGN) {             // 默认参数：必须是字面量
                advance();
                p.def = literalOnly();
                // 可选参数之后不能再有必传参数（否则无法调用）
                sawDefault = true;
            } else if (sawDefault) {
                err("带默认值的参数必须放在最后（'" + p.name + "' 在可选参数之后，必须是可选的）");
            }
            out.push_back(std::move(p));
            if (cur.type == TK_COMMA) { advance(); continue; }
            break;
        }
        expect(TK_RPAREN, ")");
    }
    bool sawDefault = false;

    ExprP literalOnly() {
        switch (cur.type) {
            case TK_NUM: {
                auto e = mk<NumLit>(); mark(e.get());
                e->d = cur.num; e->i = cur.ival; e->isFloat = cur.isFloat; advance(); return e;
            }
            case TK_STR: {
                auto e = mk<StrLit>(); mark(e.get()); e->s = cur.text; advance(); return e;
            }
            case TK_BOOL: {
                auto e = mk<BoolLit>(); mark(e.get()); e->v = (cur.num != 0); advance(); return e;
            }
            case TK_NULL: {
                auto e = mk<NullLit>(); mark(e.get()); advance(); return e;
            }
            default: err("参数默认值只允许字面量（数字 / 字符串 / true / false / null）");
        }
    }

    // 语句
    StmtP stmt() {
        switch (cur.type) {
            case TK_LBRACE:   { auto b = mk<Block>(); markS(b.get()); advance();
                                b->stmts = blockBody(); return b; }
            case TK_LOCAL:    return localDecl();
            case TK_IF:       return ifStmt();
            case TK_WHILE:    return whileStmt();
            case TK_DO:       return doWhileStmt();
            case TK_FOR:      return forStmt();
            case TK_RETURN:   return returnStmt();
            case TK_BREAK:
            case TK_CONTINUE: {
                bool isBr = (cur.type == TK_BREAK);
                Token bk = cur; advance();
                std::unique_ptr<Stmt> s;
                if (isBr) { auto b = mk<BreakStmt>(); b->level = loopLevel(bk); s = std::move(b); }
                else      { auto c = mk<ContinueStmt>(); c->level = loopLevel(bk); s = std::move(c); }
                markS(s.get()); return s;
            }
            case TK_FUNC:     { if (peek().type == TK_IDENT && peek(1).type == TK_LPAREN) return funcDef();
                                break; }
            case TK_DEFER:    return deferStmt();
            case TK_TRY:      return tryStmt();
            case TK_SEMI:     advance(); return nullptr;      // 空语句
            default:          break;
        }
        return exprOrAssignStmt();
    }

    // break / continue 后可带层数：break 2 = 作用于外层第 2 层循环
    int loopLevel(const Token& kw) {
        int lv = 1;
        if (cur.type == TK_NUM && !cur.isFloat) {
            if (cur.ival < 1) errAt(kw, "break/continue 的层数必须 >= 1");
            lv = (int)cur.ival; advance();
        }
        return lv;
    }
    std::vector<StmtP> blockBody() {          // 已在 '{' 之后
        std::vector<StmtP> out;
        while (cur.type != TK_RBRACE && cur.type != TK_EOF) {
            StmtP s = stmt();
            if (s) out.push_back(std::move(s));
        }
        expect(TK_RBRACE, "}");
        return out;
    }

    StmtP localDecl() {
        Token kw = cur; advance();                 // local
        auto s = mk<LocalDecl>(); s->line = kw.line; s->col = kw.col;
        for (;;) {
            if (cur.type != TK_IDENT) err("local 后需要变量名");
            s->names.push_back(cur.text); advance();
            if (cur.type == TK_COMMA) { advance(); continue; }
            break;
        }
        if (cur.type == TK_ASSIGN) {
            advance();
            for (;;) {
                s->values.push_back(expr());
                if (cur.type == TK_COMMA) { advance(); continue; }
                break;
            }
        }
        return s;
    }

    StmtP exprOrAssignStmt() {
        Token start = cur;
        ExprP lhs = expr();

        // 复合赋值：a += 1 / t.x -= 1
        if (cur.type == TK_ADDEQ || cur.type == TK_SUBEQ ||
            cur.type == TK_MULEQ || cur.type == TK_DIVEQ) {
            auto s = mk<CompoundAssign>(); s->line = start.line; s->col = start.col;
            s->op = cur.type; s->target = std::move(lhs); advance();
            s->value = expr();
            return s;
        }
        // 普通赋值：a = 1 / a, b = b, a（左侧必须可写）
        if (cur.type == TK_ASSIGN || cur.type == TK_COMMA) {
            auto s = mk<Assign>(); s->line = start.line; s->col = start.col;
            s->targets.push_back(std::move(lhs));
            while (cur.type == TK_COMMA) {               // 其余赋值目标
                advance();
                s->targets.push_back(expr());
            }
            expect(TK_ASSIGN, "=");
            for (;;) {
                s->values.push_back(expr());
                if (cur.type == TK_COMMA) { advance(); continue; }
                break;
            }
            return s;
        }
        auto s = mk<ExprStmt>(); s->line = start.line; s->col = start.col;
        s->e = std::move(lhs);
        return s;
    }

    // 免花括号的单语句分支：if (c) return 0 / while (c) i += 1
    std::vector<StmtP> bodyOrSingle() {
        if (cur.type == TK_LBRACE) { advance(); return blockBody(); }
        std::vector<StmtP> v;
        v.push_back(stmt());
        return v;
    }
    StmtP ifStmt() {
        Token kw = cur; advance();
        auto s = mk<IfStmt>(); s->line = kw.line; s->col = kw.col;
        s->primary.cond = expr();
        s->primary.body = bodyOrSingle();
        while (cur.type == TK_ELSE && peek().type == TK_IF) {
            advance(); advance();                  // else if
            IfStmt::Branch b;
            b.cond = expr();
            b.body = bodyOrSingle();
            s->elifs.push_back(std::move(b));
        }
        if (cur.type == TK_ELSE) {
            advance();
            if (cur.type == TK_IF) err("else if 要写成 'else if'（中间不能有换行以外的东西）");
            s->elseBody = bodyOrSingle();
            s->hasElse = true;
        }
        return s;
    }

    StmtP whileStmt() {
        Token kw = cur; advance();
        auto s = mk<WhileStmt>(); s->line = kw.line; s->col = kw.col;
        s->cond = expr();
        s->body = bodyOrSingle();
        return s;
    }

    StmtP doWhileStmt() {
        Token kw = cur; advance();                 // do
        auto s = mk<DoWhileStmt>(); s->line = kw.line; s->col = kw.col;
        expect(TK_LBRACE, "{");
        s->body = blockBody();
        expect(TK_WHILE, "while");
        s->cond = expr();
        return s;
    }

    StmtP forStmt() {
        Token kw = cur; advance();                 // for
        // 数值 for：for i = a, b [, step]
        if (cur.type == TK_IDENT && peek().type == TK_ASSIGN) {
            auto s = mk<ForNumStmt>(); s->line = kw.line; s->col = kw.col;
            s->name = cur.text; advance(); advance();
            s->start = expr();
            expect(TK_COMMA, ",");
            s->end = expr();
            if (cur.type == TK_COMMA) { advance(); s->stepv = expr(); }
            // 步长为 0 会死循环：字面量能看出来的直接编译期拒绝
            if (auto* nl = dynamic_cast<NumLit*>(s->stepv.get()))
                if (!nl->isFloat && nl->i == 0)
                    errAt(kw, "for 步长不能为 0（会导致死循环）");
            s->body = bodyOrSingle();
            return s;
        }
        auto s = mk<ForInStmt>(); s->line = kw.line; s->col = kw.col;
        if (cur.type != TK_IDENT) err("for 后需要循环变量名");
        s->kName = cur.text; advance();
        if (cur.type == TK_COMMA) { advance();
            if (cur.type != TK_IDENT) err("for 后需要循环变量名");
            s->vName = cur.text; advance(); }
        expect(TK_IN, "in");
        s->iter = expr();
        s->body = bodyOrSingle();
        return s;
    }

    StmtP returnStmt() {
        Token kw = cur; advance();
        auto s = mk<ReturnStmt>(); s->line = kw.line; s->col = kw.col;
        if (cur.type == TK_RBRACE || cur.type == TK_SEMI || cur.type == TK_EOF) return s;
        for (;;) {
            s->values.push_back(expr());
            if (cur.type == TK_COMMA) { advance(); continue; }
            break;
        }
        return s;
    }

    StmtP deferStmt() {
        Token kw = cur; advance();
        auto s = mk<DeferStmt>(); s->line = kw.line; s->col = kw.col;
        s->call = postfixExpr();                   // 必须是调用形式
        if (!dynamic_cast<Call*>(s->call.get()))
            errAt(kw, "defer 后面必须是一个函数调用");
        return s;
    }

    StmtP tryStmt() {
        Token kw = cur; advance();
        auto s = mk<TryStmt>(); s->line = kw.line; s->col = kw.col;
        expect(TK_LBRACE, "{");
        s->body = blockBody();
        if (cur.type != TK_CATCH && cur.type != TK_FINALLY) errAt(kw, "语法错误: 期望 catch，得到 '{'");
        if (cur.type == TK_CATCH) {
            advance();
            s->hasCatch = true;
            if (cur.type == TK_LPAREN) {
                advance();
                if (cur.type != TK_IDENT) err("catch 后需要异常变量名");
                s->catchVar = cur.text; advance();
                expect(TK_RPAREN, ")");
            }
            expect(TK_LBRACE, "{");
            s->catchBody = blockBody();
        }
        if (cur.type == TK_FINALLY) {
            advance();
            s->hasFinally = true;
            expect(TK_LBRACE, "{");
            s->finallyBody = blockBody();
        }
        return s;
    }

    // 表达式（优先级自低向高）
    ExprP expr() { return ternary(); }

    ExprP ternary() {
        ExprP c = orExpr();
        if (cur.type != TK_QUESTION) return c;
        Token t = cur; advance();
        auto e = mk<Ternary>(); e->line = t.line; e->col = t.col;
        e->c = std::move(c);
        e->a = expr();
        expect(TK_COLON, ":");
        e->b = expr();
        return e;
    }

    ExprP orExpr()  { return binLevel(&Parser::andExpr,  { TK_OR }); }
    ExprP andExpr() { return binLevel(&Parser::notExpr,  { TK_AND }); }

    ExprP binLevel(ExprP (Parser::*sub)(), std::initializer_list<TK> ops) {
        ExprP l = (this->*sub)();
        for (;;) {
            bool hit = false;
            for (TK o : ops) if (cur.type == o) { hit = true; break; }
            if (!hit) return l;
            Token t = cur; advance();
            auto b = mk<Binary>(); b->line = t.line; b->col = t.col;
            b->op = t.type; b->l = std::move(l); b->r = (this->*sub)();
            l = std::move(b);
        }
    }

    ExprP notExpr() {
        if (cur.type == TK_NOT) {
            Token t = cur; advance();
            auto e = mk<Unary>(); e->line = t.line; e->col = t.col;
            e->op = TK_NOT; e->e = notExpr(); return e;
        }
        return compare();
    }

    // 链式比较：a < b < c ≡ (a<b) and (b<c)，中间操作数只求值一次。
    // 保留成 Chain 节点 —— 展开成 and 会让 b 求值两遍。
    ExprP compare() {
        auto startsCmp = [&]() {
            if (cur.type >= TK_EQ && cur.type <= TK_GE) return true;
            if (cur.type == TK_IN) return true;
            if (cur.type == TK_NOT && peek().type == TK_IN) return true;
            return false;
        };
        ExprP first = bitOrExpr();
        if (!startsCmp()) return first;

        Token t = cur;
        auto ch = mk<Chain>(); ch->line = t.line; ch->col = t.col;
        ch->operands.push_back(std::move(first));
        for (;;) {
            bool negate = false, isIn = false;
            TK op = cur.type;
            if (cur.type == TK_NOT) { negate = true; advance(); }
            if (cur.type == TK_IN) { isIn = true; op = TK_IN; advance(); }
            else if (cur.type >= TK_EQ && cur.type <= TK_GE) { op = cur.type; advance(); }
            else break;
            ch->ops.push_back(op);
            ch->negates.push_back(negate);
            (void)isIn;
            ch->operands.push_back(bitOrExpr());
            if (!startsCmp()) break;
        }
        return ch;
    }

    ExprP bitOrExpr()  { return binLevel(&Parser::bitXorExpr, { TK_PIPE }); }
    ExprP bitXorExpr() { return binLevel(&Parser::bitAndExpr, { TK_CARET }); }
    ExprP bitAndExpr() { return binLevel(&Parser::shiftExpr,  { TK_AMP }); }
    ExprP shiftExpr()  { return binLevel(&Parser::addsub,     { TK_SHL, TK_SHR }); }
    ExprP addsub()     { return binLevel(&Parser::muldiv,     { TK_PLUS, TK_MINUS }); }
    ExprP muldiv()     { return binLevel(&Parser::unary,      { TK_MUL, TK_DIV, TK_MOD }); }

    ExprP unary() {
        if (cur.type == TK_MINUS || cur.type == TK_TILDE || cur.type == TK_NOT) {
            Token t = cur; advance();
            auto e = mk<Unary>(); e->line = t.line; e->col = t.col;
            e->op = t.type; e->e = unary(); return e;
        }
        return power();
    }

    ExprP power() {
        ExprP base = postfixExpr();
        if (cur.type == TK_POW) {                  // 右结合
            Token t = cur; advance();
            auto b = mk<Binary>(); b->line = t.line; b->col = t.col;
            b->op = TK_POW; b->l = std::move(std::move(base)); b->r = unary();
            return b;
        }
        return base;
    }

    // 后缀：. 字段 / [] 下标或切片 / () 调用
    ExprP postfixExpr() {
        ExprP e = primary();
        for (;;) {
            if (cur.type == TK_DOT) {
                Token t = cur; advance();
                if (cur.type != TK_IDENT) err("点号后需要字段名");
                auto f = mk<Field>(); f->line = t.line; f->col = t.col;
                f->obj = std::move(e); f->name = cur.text; advance();
                e = std::move(f);
            } else if (cur.type == TK_LBRACKET) {
                Token t = cur;
                if (isSliceAhead()) { e = finishSlice(std::move(e), t); }
                else {
                    advance();
                    auto ix = mk<Index>(); ix->line = t.line; ix->col = t.col;
                    ix->obj = std::move(e); ix->key = expr();
                    expect(TK_RBRACKET, "]");
                    e = std::move(ix);
                }
            } else if (cur.type == TK_LPAREN) {
                Token t = cur; advance();
                auto c = mk<Call>(); c->line = t.line; c->col = t.col;
                c->callee = std::move(e);
                if (cur.type != TK_RPAREN) {
                    for (;;) {
                        c->args.push_back(expr());
                        if (cur.type == TK_COMMA) { advance(); continue; }
                        break;
                    }
                }
                c->endLine = cur.line; c->endCol = cur.col;   // ')' 自己的位置（字节列）
                expect(TK_RPAREN, ")");
                e = std::move(c);
            } else break;
        }
        return e;
    }

    // 切片判定：`[` 之后紧跟 `:`（或与 `]` 之间有表达式后再 `:`）
    // 三元 `a[c ? 1 : 2]` 不会误判 —— 那个 `:` 在更深的位置且前面是 `?`
    bool isSliceAhead() {
        if (cur.type != TK_LBRACKET) return false;
        if (peek().type == TK_COLON) return true;      // a[:2]
        if (peek().type == TK_RBRACKET) return false;  // a[] → 报错交给下面
        // a[e:...] / a[e] —— 扫到同层的 ':' 才算切片
        int depth = 0;
        for (int i = 0; ; i++) {
            TK t = peek(i).type;
            if (t == TK_EOF) return false;
            if (t == TK_LPAREN || t == TK_LBRACKET || t == TK_LBRACE) { depth++; continue; }
            if (t == TK_RPAREN || t == TK_RBRACKET || t == TK_RBRACE) {
                if (depth == 0) return false;
                depth--; continue;
            }
            // 三元 a[c ? 1 : 2] 里的 ':' 不是切片分隔符 —— 见到 '?' 就排除切片
            if (t == TK_QUESTION) return false;
            if (t == TK_COLON && depth == 0) return true;
            if (t == TK_COMMA && depth == 0) return false;
        }
    }

    ExprP finishSlice(ExprP obj, const Token& lb) {
        advance();                                   // '['
        auto s = mk<Slice>(); s->line = lb.line; s->col = lb.col;
        s->obj = std::move(obj);
        if (cur.type != TK_COLON) s->start = expr();
        expect(TK_COLON, ":");
        if (cur.type != TK_COLON && cur.type != TK_RBRACKET) s->end = expr();
        if (cur.type == TK_COLON) {
            advance();
            if (cur.type != TK_RBRACKET) s->step = expr();
        }
        expect(TK_RBRACKET, "]");
        return s;
    }

    // 初等表达式
    ExprP primary() {
        Token t = cur;
        switch (cur.type) {
            case TK_NUM: {
                auto e = mk<NumLit>(); e->line = t.line; e->col = t.col;
                e->d = cur.num; e->i = cur.ival; e->isFloat = cur.isFloat; advance(); return e;
            }
            case TK_STR: {
                auto e = mk<StrLit>(); e->line = t.line; e->col = t.col;
                e->s = cur.text; advance(); return e;
            }
            case TK_BOOL: {
                auto e = mk<BoolLit>(); e->line = t.line; e->col = t.col;
                e->v = (cur.num != 0); advance(); return e;
            }
            case TK_NULL: {
                auto e = mk<NullLit>(); e->line = t.line; e->col = t.col; advance(); return e;
            }
            case TK_FUNC: {                          // 闭包表达式（可带自引用名）
                auto lam = lambdaExpr();
                lam->line = t.line; lam->col = t.col; return lam;
            }
            case TK_LBRACKET: {                      // 列表推导
                auto c = mk<Comprehension>(); c->line = t.line; c->col = t.col;
                comprehension(c.get(), false); return c;
            }
            case TK_LBRACE: {                        // 表字面量 / 字典推导
                if (isDictCompr()) {
                    auto c = mk<Comprehension>(); c->line = t.line; c->col = t.col;
                    comprehension(c.get(), true); return c;
                }
                auto tb = mk<TableLit>(); tb->line = t.line; tb->col = t.col;
                tableLiteral(tb.get()); return tb;
            }
            case TK_LPAREN: {
                advance();
                ExprP e = expr();
                expect(TK_RPAREN, ")");
                return e;
            }
            case TK_IDENT: {
                auto e = mk<Ident>(); e->line = t.line; e->col = t.col;
                e->name = cur.text; advance(); return e;
            }
            default: break;
        }
        err("语法错误: 无法解析的表达式");
    }

    ExprP lambdaExpr() {
        advance();                                   // func
        auto lam = mk<Lambda>();
        if (cur.type == TK_IDENT && peek().type == TK_LPAREN) {
            lam->name = cur.text; advance();         // 自引用名（只在自己体内可见）
        }
        expect(TK_LPAREN, "(");
        parseParamList(lam->params);
        expect(TK_LBRACE, "{");
        lam->body = blockBody();
        return lam;
    }

    void tableLiteral(TableLit* tb) {
        advance();                                   // '{'
        while (cur.type != TK_RBRACE) {
            TableItem it;
            // 统一处理：先解析一个候选表达式，再看它后面是不是 '='
            // · `a = 1` → 候选是 Ident，转成字符串键
            // · `"k" = 1` → 候选是 StrLit，直接当键
            // · `idx[1] = "a"` → 候选是 Index，当计算键
            // · `[expr] = v` → 方括号形式的计算键
            // · 其余 → 就是数组段的值
            if (cur.type == TK_LBRACKET) {
                advance();
                it.key = expr();
                expect(TK_RBRACKET, "]");
                expect(TK_ASSIGN, "=");
            } else {
                // 必须是完整 expr()：数组段元素可以是 `1 + 2 * 3` 这样的复合表达式。
                // `=` 不是二元运算符，所以 `{a = 1}` 里 expr() 只会吃掉 `a`，安全。
                ExprP cand = expr();
                if (cur.type == TK_ASSIGN) {
                    advance();
                    if (auto* id = dynamic_cast<Ident*>(cand.get()))
                        { auto k = mk<StrLit>(); k->line = id->line; k->col = id->col;
                          k->s = id->name; it.key = std::move(k); }
                    else if (dynamic_cast<Index*>(cand.get()))
                        it.key = std::move(cand);          // idx[1] = v 计算键
                    else err("表达式没写完");              // "k" = v 这类写法不合法
                } else {
                    it.value = std::move(cand);
                    tb->items.push_back(std::move(it));
                    if (cur.type == TK_COMMA) { advance(); continue; }
                    break;
                }
            }
            it.value = expr();
            tb->items.push_back(std::move(it));
            if (cur.type == TK_COMMA) { advance(); continue; }
            break;
        }
        expect(TK_RBRACE, "}");
    }

    bool isDictCompr() {
        // '{' 之后扫描：遇到同层 ':' 且没先遇到 'for' → 字典推导
        int depth = 0;
        for (int i = 0; ; i++) {
            TK t = peek(i).type;
            if (t == TK_EOF) return false;
            if (t == TK_LBRACE || t == TK_LBRACKET || t == TK_LPAREN) { depth++; continue; }
            if (t == TK_RBRACE || t == TK_RBRACKET || t == TK_RPAREN) {
                if (depth == 0) return false;
                depth--; continue;
            }
            if (depth == 0 && t == TK_FOR) return true;
        }
    }

    // 推导：[e for x in t if c] / {k : v for x in t}
    void comprehension(Comprehension* c, bool isDict) {
        advance();                                   // '[' 或 '{'
        c->isDict = isDict;

        // 收集"输出表达式"的 token，稍后 replay 解析
        std::vector<Token> valTok = scanComprExpr(false, isDict);
        std::vector<Token> keyTok;
        if (isDict) {
            // 字典推导：扫到第一个顶层':' 为止的是键
            keyTok = valTok;
            if (cur.type == TK_COLON) advance();      // 跳过分隔键值对的 ':'
            valTok = scanComprExpr(false, false);
        }

        // for 子句
        for (;;) {
            if (cur.type != TK_FOR) break;
            advance();
            ComprItem it;
            if (cur.type == TK_IDENT && peek().type == TK_ASSIGN) {
                it.isNumeric = true;
                it.nName = cur.text; advance(); advance();
                std::vector<Token> a = scanComprExpr(true, false);
                expect(TK_COMMA, ",");
                std::vector<Token> b = scanComprExpr(true, false);
                std::vector<Token> st;
                if (cur.type == TK_COMMA) { advance(); st = scanComprExpr(true, false); }
                { auto save = valTok; (void)save; }
                // 三段延迟解析（保存 token 副本，稍后在发射期按作用域 replay）
                it.start = parseTokExpr(a);
                it.stop  = parseTokExpr(b);
                if (!st.empty()) {
                    it.stepv = parseTokExpr(st);
                    if (auto* nl = dynamic_cast<NumLit*>(it.stepv.get()))
                        if (!nl->isFloat && nl->i == 0)
                            err("for 步长不能为 0（会导致死循环）");
                }
            } else {
                if (cur.type != TK_IDENT) err("for 后需要循环变量名");
                it.kName = cur.text; advance();
                if (cur.type == TK_COMMA) {
                    advance();
                    if (cur.type != TK_IDENT) err("for 后需要循环变量名");
                    it.vName = cur.text; advance();
                }
                expect(TK_IN, "in");
                it.iter = parseTokExpr(scanComprExpr(false, false));
            }
            if (cur.type == TK_IF) {
                advance();
                it.cond = parseTokExpr(scanComprExpr(false, false));
            }
            c->items.push_back(std::move(it));
        }
        if (c->items.empty()) err("推导必须以 for 开头，例如 [x * 2 for x in t]");
        expect(isDict ? TK_RBRACE : TK_RBRACKET, isDict ? "}" : "]");

        // 输出表达式也延迟解析（要能引用所有层的循环变量）
        if (isDict) c->key = parseTokExpr(keyTok);
        c->value = parseTokExpr(valTok);
    }

    // 把一段 token 当作独立表达式解析（推导里"先收集、后解析"用）
    ExprP parseTokExpr(const std::vector<Token>& toks) {
        if (toks.empty()) return nullptr;
        std::deque<Token> save = std::move(q);
        Token saveCur = cur;
        q.clear();
        for (const Token& t : toks) q.push_back(t);
        // 哨兵：这段 token 是独立的小表达式，前瞻越界时不能穿透到后面的源码
        Token eof; eof.type = TK_EOF; q.push_back(eof);
        advance();
        ExprP e = expr();
        q = std::move(save);
        cur = saveCur;
        return e;
    }

    // 扫描到本层结束：遇 ',' 或 ':'（可选）或收尾括号为止
    std::vector<Token> scanComprExpr(bool stopAtComma, bool stopAtColon) {
        std::vector<Token> out;
        int depth = 0;
        for (;;) {
            TK t = cur.type;
            if (t == TK_EOF) break;
            if (t == TK_LPAREN || t == TK_LBRACKET || t == TK_LBRACE) depth++;
            else if (t == TK_RPAREN || t == TK_RBRACKET || t == TK_RBRACE) {
                if (depth == 0) break;
                depth--;
            } else if (depth == 0) {
                if (stopAtComma && t == TK_COMMA) break;
                if (stopAtColon && t == TK_COLON) break;
                if (t == TK_FOR || t == TK_IF) break;
            }
            out.push_back(cur);
            advance();
        }
        return out;
    }
};

} // namespace aeparse

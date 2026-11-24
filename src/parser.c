#include "ash.h"
#include <ctype.h>

typedef struct {
    Lexer lx;
    Token cur;
    int status;
    char error[256];
} Parser;

static Node *node_new(NodeKind kind)
{
    Node *n = xmalloc(sizeof(Node));
    memset(n, 0, sizeof(Node));
    n->kind = kind;
    return n;
}

static void advance(Parser *p)
{
    word_free(&p->cur.word);
    memset(&p->cur, 0, sizeof(Token));
    word_init(&p->cur.word);
    p->cur.fd = -1;
    int rc = lexer_next(&p->lx, &p->cur);
    if (rc < 0) {
        if (p->lx.incomplete)
            p->status = 1;
        else {
            p->status = 2;
            snprintf(p->error, sizeof(p->error), "%s", p->lx.error);
        }
    }
}

static Word take_word(Parser *p)
{
    Word w = p->cur.word;
    word_init(&p->cur.word);
    return w;
}

static void token_desc(Parser *p, char *buf, size_t size)
{
    if (p->cur.kind == TOK_WORD) {
        char *raw = word_raw_text(&p->cur.word);
        snprintf(buf, size, "`%s'", raw);
        free(raw);
        return;
    }
    const char *names[] = {
        [TOK_NEWLINE] = "newline", [TOK_EOF] = "end of file",
        [TOK_SEMI] = ";", [TOK_DSEMI] = ";;", [TOK_AMP] = "&",
        [TOK_AND_IF] = "&&", [TOK_OR_IF] = "||", [TOK_PIPE] = "|",
        [TOK_LPAREN] = "(", [TOK_RPAREN] = ")", [TOK_REDIR] = "redirection"
    };
    snprintf(buf, size, "%s", names[p->cur.kind]);
}

static void fail(Parser *p, const char *msg)
{
    if (p->status)
        return;
    if (p->cur.kind == TOK_EOF) {
        p->status = 1;
        return;
    }
    p->status = 2;
    char desc[128];
    token_desc(p, desc, sizeof(desc));
    snprintf(p->error, sizeof(p->error), "%s near unexpected token %s",
             msg, desc);
}

static void skip_newlines(Parser *p)
{
    while (p->cur.kind == TOK_NEWLINE)
        advance(p);
}

static int cur_word_is(Parser *p, const char *text)
{
    if (p->cur.kind != TOK_WORD)
        return 0;
    char *raw = word_raw_text(&p->cur.word);
    int match = strcmp(raw, text) == 0;
    free(raw);
    return match;
}

static int is_list_terminator(Parser *p)
{
    if (p->cur.kind == TOK_EOF || p->cur.kind == TOK_RPAREN ||
        p->cur.kind == TOK_DSEMI)
        return 1;
    return cur_word_is(p, "then") || cur_word_is(p, "elif") ||
           cur_word_is(p, "else") || cur_word_is(p, "fi") ||
           cur_word_is(p, "do") || cur_word_is(p, "done") ||
           cur_word_is(p, "esac") || cur_word_is(p, "}");
}

static int expect_word(Parser *p, const char *text)
{
    if (!cur_word_is(p, text)) {
        char msg[128];
        snprintf(msg, sizeof(msg), "syntax error: expected `%s'", text);
        fail(p, msg);
        return 0;
    }
    advance(p);
    return 1;
}

static int parse_redir(Parser *p, Node *n)
{
    Redirect r;
    memset(&r, 0, sizeof(r));
    r.kind = p->cur.redir_kind;
    r.fd = p->cur.fd;
    r.heredoc = p->cur.heredoc;
    p->cur.heredoc = NULL;
    if (r.heredoc)
        r.heredoc->claimed = 1;
    word_init(&r.target);
    advance(p);
    if (r.kind != REDIR_HEREDOC) {
        if (p->cur.kind != TOK_WORD) {
            fail(p, "syntax error: expected filename after redirection");
            word_free(&r.target);
            if (r.heredoc) {
                free(r.heredoc->text);
                free(r.heredoc);
            }
            return -1;
        }
        r.target = take_word(p);
        advance(p);
    }
    n->redirs = xrealloc(n->redirs, (n->redir_count + 1) * sizeof(Redirect));
    n->redirs[n->redir_count++] = r;
    return 0;
}

static void collect_trailing_redirs(Parser *p, Node *n)
{
    while (p->cur.kind == TOK_REDIR)
        if (parse_redir(p, n) < 0)
            return;
}

static int is_assignment_text(const char *raw)
{
    const char *eq = strchr(raw, '=');
    if (!eq || eq == raw)
        return 0;
    if (!isalpha((unsigned char)raw[0]) && raw[0] != '_')
        return 0;
    for (const char *c = raw + 1; c < eq; c++)
        if (!isalnum((unsigned char)*c) && *c != '_')
            return 0;
    return 1;
}

static Node *parse_command(Parser *p);
static Node *parse_list(Parser *p);

static Node *parse_if(Parser *p)
{
    advance(p);
    Node *n = node_new(NODE_IF);
    for (;;) {
        Node *cond = parse_list(p);
        if (!cond) {
            fail(p, "syntax error: expected condition");
            node_free(n);
            return NULL;
        }
        if (!expect_word(p, "then")) {
            node_free(cond);
            node_free(n);
            return NULL;
        }
        Node *body = parse_list(p);
        if (!body) {
            fail(p, "syntax error: expected body after `then'");
            node_free(cond);
            node_free(n);
            return NULL;
        }
        int i = n->as.if_node.pair_count++;
        n->as.if_node.conds = xrealloc(n->as.if_node.conds,
                                       n->as.if_node.pair_count * sizeof(Node *));
        n->as.if_node.bodies = xrealloc(n->as.if_node.bodies,
                                        n->as.if_node.pair_count * sizeof(Node *));
        n->as.if_node.conds[i] = cond;
        n->as.if_node.bodies[i] = body;
        if (cur_word_is(p, "elif")) {
            advance(p);
            continue;
        }
        break;
    }
    if (cur_word_is(p, "else")) {
        advance(p);
        n->as.if_node.else_body = parse_list(p);
        if (!n->as.if_node.else_body) {
            fail(p, "syntax error: expected body after `else'");
            node_free(n);
            return NULL;
        }
    }
    if (!expect_word(p, "fi")) {
        node_free(n);
        return NULL;
    }
    return n;
}

static Node *parse_loop(Parser *p, int until)
{
    advance(p);
    Node *cond = parse_list(p);
    if (!cond) {
        fail(p, "syntax error: expected loop condition");
        return NULL;
    }
    if (!expect_word(p, "do")) {
        node_free(cond);
        return NULL;
    }
    Node *body = parse_list(p);
    if (!body) {
        fail(p, "syntax error: expected loop body");
        node_free(cond);
        return NULL;
    }
    if (!expect_word(p, "done")) {
        node_free(cond);
        node_free(body);
        return NULL;
    }
    Node *n = node_new(NODE_WHILE);
    n->as.loop.cond = cond;
    n->as.loop.body = body;
    n->as.loop.until = until;
    return n;
}

static Node *parse_for(Parser *p)
{
    advance(p);
    if (p->cur.kind != TOK_WORD) {
        fail(p, "syntax error: expected variable name after `for'");
        return NULL;
    }
    char *name = word_raw_text(&p->cur.word);
    advance(p);
    skip_newlines(p);
    Node *n = node_new(NODE_FOR);
    n->as.for_node.name = name;
    if (cur_word_is(p, "in")) {
        advance(p);
        n->as.for_node.has_items = 1;
        while (p->cur.kind == TOK_WORD) {
            int i = n->as.for_node.item_count++;
            n->as.for_node.items = xrealloc(n->as.for_node.items,
                                            n->as.for_node.item_count * sizeof(Word));
            n->as.for_node.items[i] = take_word(p);
            advance(p);
        }
    }
    if (p->cur.kind == TOK_SEMI || p->cur.kind == TOK_NEWLINE) {
        advance(p);
        skip_newlines(p);
    } else if (!cur_word_is(p, "do")) {
        fail(p, "syntax error: expected `;' or `do' in for loop");
        node_free(n);
        return NULL;
    }
    if (!expect_word(p, "do")) {
        node_free(n);
        return NULL;
    }
    n->as.for_node.body = parse_list(p);
    if (!n->as.for_node.body) {
        fail(p, "syntax error: expected for loop body");
        node_free(n);
        return NULL;
    }
    if (!expect_word(p, "done")) {
        node_free(n);
        return NULL;
    }
    return n;
}

static Node *parse_case(Parser *p)
{
    advance(p);
    if (p->cur.kind != TOK_WORD) {
        fail(p, "syntax error: expected word after `case'");
        return NULL;
    }
    Node *n = node_new(NODE_CASE);
    n->as.case_node.subject = take_word(p);
    advance(p);
    skip_newlines(p);
    if (!expect_word(p, "in")) {
        node_free(n);
        return NULL;
    }
    skip_newlines(p);
    while (!cur_word_is(p, "esac")) {
        if (p->cur.kind == TOK_EOF) {
            fail(p, "syntax error: expected `esac'");
            node_free(n);
            return NULL;
        }
        if (p->cur.kind == TOK_LPAREN)
            advance(p);
        CaseArm arm;
        memset(&arm, 0, sizeof(arm));
        if (p->cur.kind != TOK_WORD) {
            fail(p, "syntax error: expected case pattern");
            node_free(n);
            return NULL;
        }
        for (;;) {
            arm.patterns = xrealloc(arm.patterns,
                                    (arm.pattern_count + 1) * sizeof(Word));
            arm.patterns[arm.pattern_count++] = take_word(p);
            advance(p);
            if (p->cur.kind == TOK_PIPE) {
                advance(p);
                if (p->cur.kind != TOK_WORD) {
                    fail(p, "syntax error: expected pattern after `|'");
                    for (int i = 0; i < arm.pattern_count; i++)
                        word_free(&arm.patterns[i]);
                    free(arm.patterns);
                    node_free(n);
                    return NULL;
                }
                continue;
            }
            break;
        }
        if (p->cur.kind != TOK_RPAREN) {
            fail(p, "syntax error: expected `)' after case pattern");
            for (int i = 0; i < arm.pattern_count; i++)
                word_free(&arm.patterns[i]);
            free(arm.patterns);
            node_free(n);
            return NULL;
        }
        advance(p);
        skip_newlines(p);
        arm.body = parse_list(p);
        int i = n->as.case_node.arm_count++;
        n->as.case_node.arms = xrealloc(n->as.case_node.arms,
                                        n->as.case_node.arm_count * sizeof(CaseArm));
        n->as.case_node.arms[i] = arm;
        if (p->cur.kind == TOK_DSEMI) {
            advance(p);
            skip_newlines(p);
            continue;
        }
        break;
    }
    if (!expect_word(p, "esac")) {
        node_free(n);
        return NULL;
    }
    return n;
}

static Node *parse_group(Parser *p)
{
    advance(p);
    skip_newlines(p);
    Node *body = parse_list(p);
    if (!body) {
        fail(p, "syntax error: expected commands after `{'");
        return NULL;
    }
    if (!expect_word(p, "}")) {
        node_free(body);
        return NULL;
    }
    Node *n = node_new(NODE_GROUP);
    n->as.body = body;
    return n;
}

static Node *parse_subshell(Parser *p)
{
    advance(p);
    skip_newlines(p);
    Node *body = parse_list(p);
    if (!body) {
        fail(p, "syntax error: expected commands after `('");
        return NULL;
    }
    if (p->cur.kind != TOK_RPAREN) {
        fail(p, "syntax error: expected `)'");
        node_free(body);
        return NULL;
    }
    advance(p);
    Node *n = node_new(NODE_SUBSHELL);
    n->as.body = body;
    return n;
}

static Node *parse_function_body(Parser *p, char *name)
{
    skip_newlines(p);
    Node *body = parse_command(p);
    if (!body) {
        free(name);
        return NULL;
    }
    Node *n = node_new(NODE_FUNCDEF);
    n->as.funcdef.name = name;
    n->as.funcdef.body = body;
    return n;
}

static Node *parse_command(Parser *p)
{
    if (p->status)
        return NULL;
    if (p->cur.kind == TOK_WORD) {
        char *raw = word_raw_text(&p->cur.word);
        Node *compound = NULL;
        if (strcmp(raw, "if") == 0)
            compound = parse_if(p);
        else if (strcmp(raw, "while") == 0)
            compound = parse_loop(p, 0);
        else if (strcmp(raw, "until") == 0)
            compound = parse_loop(p, 1);
        else if (strcmp(raw, "for") == 0)
            compound = parse_for(p);
        else if (strcmp(raw, "case") == 0)
            compound = parse_case(p);
        else if (strcmp(raw, "{") == 0)
            compound = parse_group(p);
        else if (strcmp(raw, "function") == 0) {
            advance(p);
            if (p->cur.kind != TOK_WORD) {
                fail(p, "syntax error: expected function name");
                free(raw);
                return NULL;
            }
            char *name = word_raw_text(&p->cur.word);
            advance(p);
            if (p->cur.kind == TOK_LPAREN) {
                advance(p);
                if (p->cur.kind != TOK_RPAREN) {
                    fail(p, "syntax error: expected `)'");
                    free(name);
                    free(raw);
                    return NULL;
                }
                advance(p);
            }
            free(raw);
            Node *n = parse_function_body(p, name);
            if (n)
                collect_trailing_redirs(p, n);
            return n;
        }
        free(raw);
        if (compound) {
            collect_trailing_redirs(p, compound);
            return compound;
        }
        if (p->status)
            return NULL;
    }
    if (p->cur.kind == TOK_LPAREN) {
        Node *n = parse_subshell(p);
        if (n)
            collect_trailing_redirs(p, n);
        return n;
    }

    Node *n = node_new(NODE_COMMAND);
    for (;;) {
        if (p->cur.kind == TOK_WORD) {
            char *raw = word_raw_text(&p->cur.word);
            int is_assign = n->as.command.word_count == 0 &&
                            is_assignment_text(raw);
            free(raw);
            if (is_assign) {
                int i = n->as.command.assign_count++;
                n->as.command.assigns = xrealloc(n->as.command.assigns,
                                                 n->as.command.assign_count * sizeof(Word));
                n->as.command.assigns[i] = take_word(p);
            } else {
                int i = n->as.command.word_count++;
                n->as.command.words = xrealloc(n->as.command.words,
                                               n->as.command.word_count * sizeof(Word));
                n->as.command.words[i] = take_word(p);
            }
            advance(p);
            continue;
        }
        if (p->cur.kind == TOK_REDIR) {
            if (parse_redir(p, n) < 0) {
                node_free(n);
                return NULL;
            }
            continue;
        }
        break;
    }
    if (n->as.command.word_count == 0 && n->as.command.assign_count == 0 &&
        n->redir_count == 0) {
        node_free(n);
        if (p->cur.kind == TOK_EOF)
            p->status = 1;
        else if (!p->status)
            fail(p, "syntax error");
        return NULL;
    }
    if (n->as.command.word_count == 1 && n->as.command.assign_count == 0 &&
        n->redir_count == 0 && p->cur.kind == TOK_LPAREN) {
        advance(p);
        if (p->cur.kind != TOK_RPAREN) {
            fail(p, "syntax error: expected `)' in function definition");
            node_free(n);
            return NULL;
        }
        advance(p);
        char *name = word_raw_text(&n->as.command.words[0]);
        node_free(n);
        Node *fn = parse_function_body(p, name);
        if (fn)
            collect_trailing_redirs(p, fn);
        return fn;
    }
    return n;
}

static Node *parse_pipeline(Parser *p)
{
    int negated = 0;
    if (cur_word_is(p, "!")) {
        negated = 1;
        advance(p);
    }
    Node *first = parse_command(p);
    if (!first)
        return NULL;
    if (p->cur.kind != TOK_PIPE) {
        if (!negated)
            return first;
        Node *n = node_new(NODE_PIPELINE);
        n->as.pipeline.stages = xmalloc(sizeof(Node *));
        n->as.pipeline.stages[0] = first;
        n->as.pipeline.stage_count = 1;
        n->as.pipeline.negated = 1;
        return n;
    }
    Node *n = node_new(NODE_PIPELINE);
    n->as.pipeline.negated = negated;
    n->as.pipeline.stages = xmalloc(sizeof(Node *));
    n->as.pipeline.stages[0] = first;
    n->as.pipeline.stage_count = 1;
    while (p->cur.kind == TOK_PIPE) {
        advance(p);
        skip_newlines(p);
        Node *next = parse_command(p);
        if (!next) {
            node_free(n);
            return NULL;
        }
        int i = n->as.pipeline.stage_count++;
        n->as.pipeline.stages = xrealloc(n->as.pipeline.stages,
                                         n->as.pipeline.stage_count * sizeof(Node *));
        n->as.pipeline.stages[i] = next;
    }
    return n;
}

static Node *parse_and_or(Parser *p)
{
    Node *left = parse_pipeline(p);
    if (!left)
        return NULL;
    while (p->cur.kind == TOK_AND_IF || p->cur.kind == TOK_OR_IF) {
        int or_op = p->cur.kind == TOK_OR_IF;
        advance(p);
        skip_newlines(p);
        Node *right = parse_pipeline(p);
        if (!right) {
            node_free(left);
            return NULL;
        }
        Node *n = node_new(NODE_AND_OR);
        n->as.and_or.left = left;
        n->as.and_or.right = right;
        n->as.and_or.or_op = or_op;
        left = n;
    }
    return left;
}

static Node *parse_list(Parser *p)
{
    skip_newlines(p);
    Node **items = NULL;
    int *separators = NULL;
    int count = 0;
    for (;;) {
        if (is_list_terminator(p))
            break;
        Node *ao = parse_and_or(p);
        if (!ao)
            break;
        items = xrealloc(items, (count + 1) * sizeof(Node *));
        separators = xrealloc(separators, (count + 1) * sizeof(int));
        items[count] = ao;
        separators[count] = 0;
        count++;
        if (p->cur.kind == TOK_SEMI || p->cur.kind == TOK_AMP) {
            separators[count - 1] = p->cur.kind == TOK_AMP ? '&' : ';';
            advance(p);
            skip_newlines(p);
            continue;
        }
        if (p->cur.kind == TOK_NEWLINE) {
            separators[count - 1] = ';';
            advance(p);
            skip_newlines(p);
            continue;
        }
        break;
    }
    if (count == 0) {
        free(items);
        free(separators);
        return NULL;
    }
    if (count == 1 && separators[0] == 0) {
        Node *single = items[0];
        free(items);
        free(separators);
        return single;
    }
    Node *n = node_new(NODE_LIST);
    n->as.list.items = items;
    n->as.list.separators = separators;
    n->as.list.count = count;
    return n;
}

ParseOutput parse_string(const char *input)
{
    Parser p;
    memset(&p, 0, sizeof(p));
    lexer_init(&p.lx, input);
    word_init(&p.cur.word);
    p.cur.fd = -1;
    advance(&p);
    ParseOutput out;
    memset(&out, 0, sizeof(out));
    skip_newlines(&p);
    if (p.cur.kind == TOK_EOF && !p.status) {
        out.result = PARSE_OK;
        out.root = NULL;
        word_free(&p.cur.word);
        lexer_free(&p.lx);
        return out;
    }
    Node *root = parse_list(&p);
    if (root && !p.status) {
        skip_newlines(&p);
        if (p.cur.kind != TOK_EOF)
            fail(&p, "syntax error");
    }
    if (p.status == 1) {
        out.result = PARSE_INCOMPLETE;
        node_free(root);
    } else if (p.status == 2 || !root) {
        out.result = PARSE_ERROR;
        snprintf(out.error, sizeof(out.error), "%s",
                 p.error[0] ? p.error : "syntax error");
        node_free(root);
    } else {
        out.result = PARSE_OK;
        out.root = root;
    }
    word_free(&p.cur.word);
    lexer_free(&p.lx);
    return out;
}

static Redirect *redirs_clone(const Redirect *r, int count)
{
    if (count == 0)
        return NULL;
    Redirect *out = xmalloc(count * sizeof(Redirect));
    for (int i = 0; i < count; i++) {
        out[i].kind = r[i].kind;
        out[i].fd = r[i].fd;
        out[i].target = word_clone(&r[i].target);
        out[i].heredoc = NULL;
        if (r[i].heredoc) {
            out[i].heredoc = xmalloc(sizeof(HeredocBody));
            out[i].heredoc->text = xstrdup(r[i].heredoc->text);
            out[i].heredoc->quoted = r[i].heredoc->quoted;
            out[i].heredoc->claimed = 1;
        }
    }
    return out;
}

Node *node_clone(const Node *n)
{
    if (!n)
        return NULL;
    Node *out = node_new(n->kind);
    out->redirs = redirs_clone(n->redirs, n->redir_count);
    out->redir_count = n->redir_count;
    switch (n->kind) {
    case NODE_COMMAND: {
        CommandNode *c = &out->as.command;
        const CommandNode *src = &n->as.command;
        c->assign_count = src->assign_count;
        c->word_count = src->word_count;
        if (src->assign_count) {
            c->assigns = xmalloc(src->assign_count * sizeof(Word));
            for (int i = 0; i < src->assign_count; i++)
                c->assigns[i] = word_clone(&src->assigns[i]);
        }
        if (src->word_count) {
            c->words = xmalloc(src->word_count * sizeof(Word));
            for (int i = 0; i < src->word_count; i++)
                c->words[i] = word_clone(&src->words[i]);
        }
        break;
    }
    case NODE_PIPELINE:
        out->as.pipeline.negated = n->as.pipeline.negated;
        out->as.pipeline.stage_count = n->as.pipeline.stage_count;
        out->as.pipeline.stages =
            xmalloc(n->as.pipeline.stage_count * sizeof(Node *));
        for (int i = 0; i < n->as.pipeline.stage_count; i++)
            out->as.pipeline.stages[i] = node_clone(n->as.pipeline.stages[i]);
        break;
    case NODE_AND_OR:
        out->as.and_or.or_op = n->as.and_or.or_op;
        out->as.and_or.left = node_clone(n->as.and_or.left);
        out->as.and_or.right = node_clone(n->as.and_or.right);
        break;
    case NODE_LIST:
        out->as.list.count = n->as.list.count;
        out->as.list.items = xmalloc(n->as.list.count * sizeof(Node *));
        out->as.list.separators = xmalloc(n->as.list.count * sizeof(int));
        for (int i = 0; i < n->as.list.count; i++) {
            out->as.list.items[i] = node_clone(n->as.list.items[i]);
            out->as.list.separators[i] = n->as.list.separators[i];
        }
        break;
    case NODE_SUBSHELL:
    case NODE_GROUP:
        out->as.body = node_clone(n->as.body);
        break;
    case NODE_IF: {
        IfNode *dst = &out->as.if_node;
        const IfNode *src = &n->as.if_node;
        dst->pair_count = src->pair_count;
        dst->conds = xmalloc(src->pair_count * sizeof(Node *));
        dst->bodies = xmalloc(src->pair_count * sizeof(Node *));
        for (int i = 0; i < src->pair_count; i++) {
            dst->conds[i] = node_clone(src->conds[i]);
            dst->bodies[i] = node_clone(src->bodies[i]);
        }
        dst->else_body = node_clone(src->else_body);
        break;
    }
    case NODE_WHILE:
        out->as.loop.until = n->as.loop.until;
        out->as.loop.cond = node_clone(n->as.loop.cond);
        out->as.loop.body = node_clone(n->as.loop.body);
        break;
    case NODE_FOR: {
        ForNode *dst = &out->as.for_node;
        const ForNode *src = &n->as.for_node;
        dst->name = xstrdup(src->name);
        dst->has_items = src->has_items;
        dst->item_count = src->item_count;
        if (src->item_count) {
            dst->items = xmalloc(src->item_count * sizeof(Word));
            for (int i = 0; i < src->item_count; i++)
                dst->items[i] = word_clone(&src->items[i]);
        }
        dst->body = node_clone(src->body);
        break;
    }
    case NODE_CASE: {
        CaseNode *dst = &out->as.case_node;
        const CaseNode *src = &n->as.case_node;
        dst->subject = word_clone(&src->subject);
        dst->arm_count = src->arm_count;
        dst->arms = xmalloc(src->arm_count * sizeof(CaseArm));
        for (int i = 0; i < src->arm_count; i++) {
            dst->arms[i].pattern_count = src->arms[i].pattern_count;
            dst->arms[i].patterns =
                xmalloc(src->arms[i].pattern_count * sizeof(Word));
            for (int j = 0; j < src->arms[i].pattern_count; j++)
                dst->arms[i].patterns[j] = word_clone(&src->arms[i].patterns[j]);
            dst->arms[i].body = node_clone(src->arms[i].body);
        }
        break;
    }
    case NODE_FUNCDEF:
        out->as.funcdef.name = xstrdup(n->as.funcdef.name);
        out->as.funcdef.body = node_clone(n->as.funcdef.body);
        break;
    }
    return out;
}

static void redirs_free(Redirect *r, int count)
{
    for (int i = 0; i < count; i++) {
        word_free(&r[i].target);
        if (r[i].heredoc) {
            free(r[i].heredoc->text);
            free(r[i].heredoc);
        }
    }
    free(r);
}

void node_free(Node *n)
{
    if (!n)
        return;
    redirs_free(n->redirs, n->redir_count);
    switch (n->kind) {
    case NODE_COMMAND:
        for (int i = 0; i < n->as.command.assign_count; i++)
            word_free(&n->as.command.assigns[i]);
        free(n->as.command.assigns);
        for (int i = 0; i < n->as.command.word_count; i++)
            word_free(&n->as.command.words[i]);
        free(n->as.command.words);
        break;
    case NODE_PIPELINE:
        for (int i = 0; i < n->as.pipeline.stage_count; i++)
            node_free(n->as.pipeline.stages[i]);
        free(n->as.pipeline.stages);
        break;
    case NODE_AND_OR:
        node_free(n->as.and_or.left);
        node_free(n->as.and_or.right);
        break;
    case NODE_LIST:
        for (int i = 0; i < n->as.list.count; i++)
            node_free(n->as.list.items[i]);
        free(n->as.list.items);
        free(n->as.list.separators);
        break;
    case NODE_SUBSHELL:
    case NODE_GROUP:
        node_free(n->as.body);
        break;
    case NODE_IF:
        for (int i = 0; i < n->as.if_node.pair_count; i++) {
            node_free(n->as.if_node.conds[i]);
            node_free(n->as.if_node.bodies[i]);
        }
        free(n->as.if_node.conds);
        free(n->as.if_node.bodies);
        node_free(n->as.if_node.else_body);
        break;
    case NODE_WHILE:
        node_free(n->as.loop.cond);
        node_free(n->as.loop.body);
        break;
    case NODE_FOR:
        free(n->as.for_node.name);
        for (int i = 0; i < n->as.for_node.item_count; i++)
            word_free(&n->as.for_node.items[i]);
        free(n->as.for_node.items);
        node_free(n->as.for_node.body);
        break;
    case NODE_CASE:
        word_free(&n->as.case_node.subject);
        for (int i = 0; i < n->as.case_node.arm_count; i++) {
            for (int j = 0; j < n->as.case_node.arms[i].pattern_count; j++)
                word_free(&n->as.case_node.arms[i].patterns[j]);
            free(n->as.case_node.arms[i].patterns);
            node_free(n->as.case_node.arms[i].body);
        }
        free(n->as.case_node.arms);
        break;
    case NODE_FUNCDEF:
        free(n->as.funcdef.name);
        node_free(n->as.funcdef.body);
        break;
    }
    free(n);
}

char *node_command_text(Node *n)
{
    StrBuf sb;
    sb_init(&sb);
    if (!n)
        return sb_take(&sb);
    if (n->kind == NODE_COMMAND) {
        for (int i = 0; i < n->as.command.word_count; i++) {
            if (i > 0)
                sb_append_char(&sb, ' ');
            char *raw = word_raw_text(&n->as.command.words[i]);
            sb_append(&sb, raw);
            free(raw);
        }
    } else if (n->kind == NODE_PIPELINE) {
        for (int i = 0; i < n->as.pipeline.stage_count; i++) {
            if (i > 0)
                sb_append(&sb, " | ");
            char *part = node_command_text(n->as.pipeline.stages[i]);
            sb_append(&sb, part);
            free(part);
        }
    } else if (n->kind == NODE_LIST && n->as.list.count > 0) {
        char *part = node_command_text(n->as.list.items[0]);
        sb_append(&sb, part);
        free(part);
        if (n->as.list.count > 1)
            sb_append(&sb, " ...");
    } else if (n->kind == NODE_SUBSHELL || n->kind == NODE_GROUP) {
        char *part = node_command_text(n->as.body);
        sb_append(&sb, part);
        free(part);
    } else {
        sb_append(&sb, "<compound>");
    }
    return sb_take(&sb);
}
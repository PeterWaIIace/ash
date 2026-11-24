#include "ash.h"
#include <ctype.h>

extern ShellOptions options;
extern ShellState shell;

static int is_blank(int c)
{
    return c == ' ' || c == '\t';
}

static int is_operator_start(int c)
{
    return c == '|' || c == '&' || c == ';' || c == '<' || c == '>' ||
           c == '(' || c == ')';
}

static int is_name_start(int c)
{
    return isalpha(c) || c == '_';
}

static int is_name_char(int c)
{
    return isalnum(c) || c == '_';
}

void word_init(Word *w)
{
    w->items = NULL;
    w->count = 0;
    w->capacity = 0;
}

void word_free(Word *w)
{
    for (int i = 0; i < w->count; i++)
        free(w->items[i].text);
    free(w->items);
    word_init(w);
}

void word_add_piece(Word *w, PieceKind kind, int quoted, const char *text)
{
    if (w->count == w->capacity) {
        w->capacity = w->capacity ? w->capacity * 2 : 8;
        w->items = xrealloc(w->items, w->capacity * sizeof(Piece));
    }
    w->items[w->count].kind = kind;
    w->items[w->count].quoted = quoted;
    w->items[w->count].text = xstrdup(text);
    w->count++;
}

void word_append_text(Word *w, int quoted, const char *text, size_t n)
{
    if (n == 0)
        return;
    if (w->count > 0 && w->items[w->count - 1].kind == PIECE_TEXT &&
        w->items[w->count - 1].quoted == quoted) {
        Piece *p = &w->items[w->count - 1];
        size_t old = strlen(p->text);
        p->text = xrealloc(p->text, old + n + 1);
        memcpy(p->text + old, text, n);
        p->text[old + n] = '\0';
        return;
    }
    char *tmp = xmalloc(n + 1);
    memcpy(tmp, text, n);
    tmp[n] = '\0';
    word_add_piece(w, PIECE_TEXT, quoted, tmp);
    free(tmp);
}

char *word_raw_text(const Word *w)
{
    StrBuf sb;
    sb_init(&sb);
    for (int i = 0; i < w->count; i++)
        sb_append(&sb, w->items[i].text);
    return sb_take(&sb);
}

Word word_clone(const Word *w)
{
    Word out;
    word_init(&out);
    for (int i = 0; i < w->count; i++)
        word_add_piece(&out, w->items[i].kind, w->items[i].quoted,
                       w->items[i].text);
    return out;
}

void lexer_init(Lexer *lx, const char *input)
{
    lx->input = input;
    lx->pos = 0;
    lx->incomplete = 0;
    lx->error[0] = '\0';
    lx->pending_count = 0;
    lx->body_count = 0;
}

void lexer_free(Lexer *lx)
{
    for (int i = 0; i < lx->pending_count; i++)
        free(lx->pending[i].delim);
    for (int i = 0; i < lx->body_count; i++) {
        if (!lx->bodies[i]->claimed) {
            free(lx->bodies[i]->text);
            free(lx->bodies[i]);
        }
    }
}

static HeredocBody *heredoc_body_new(Lexer *lx, int quoted)
{
    HeredocBody *body = xmalloc(sizeof(HeredocBody));
    body->text = xstrdup("");
    body->quoted = quoted;
    body->claimed = 0;
    if (lx->body_count < 64)
        lx->bodies[lx->body_count++] = body;
    return body;
}

static size_t scan_quoted_region(Lexer *lx, size_t pos, char quote)
{
    size_t len = strlen(lx->input);
    pos++;
    while (pos < len) {
        char c = lx->input[pos];
        if (c == '\\' && quote == '"') {
            pos += 2;
            continue;
        }
        if (c == quote)
            return pos + 1;
        pos++;
    }
    lx->incomplete = 1;
    return len;
}

static size_t scan_balanced(Lexer *lx, size_t pos, char open, char close)
{
    size_t len = strlen(lx->input);
    int depth = 1;
    while (pos < len) {
        char c = lx->input[pos];
        if (c == '\\') {
            pos += 2;
            continue;
        }
        if (c == '\'') {
            pos = scan_quoted_region(lx, pos, '\'');
            continue;
        }
        if (c == '"') {
            pos = scan_quoted_region(lx, pos, '"');
            continue;
        }
        if (c == '$' && pos + 1 < len && lx->input[pos + 1] == '(') {
            pos = scan_balanced(lx, pos + 2, '(', ')');
            continue;
        }
        if (c == open)
            depth++;
        else if (c == close) {
            depth--;
            if (depth == 0)
                return pos + 1;
        }
        pos++;
    }
    lx->incomplete = 1;
    return len;
}

static void scan_dollar(Lexer *lx, Word *w, int quoted)
{
    size_t len = strlen(lx->input);
    size_t start = lx->pos;
    lx->pos++;
    if (lx->pos >= len) {
        word_append_text(w, quoted, "$", 1);
        return;
    }
    char c = lx->input[lx->pos];
    if (c == '(') {
        if (lx->pos + 1 < len && lx->input[lx->pos + 1] == '(') {
            size_t end = scan_balanced(lx, lx->pos + 2, '(', ')');
            if (lx->incomplete)
                return;
            if (end < len && lx->input[end] == ')')
                end++;
            else {
                lx->incomplete = 1;
                return;
            }
            size_t body_start = lx->pos + 2;
            size_t body_len = end - body_start - 2;
            char *body = xmalloc(body_len + 1);
            memcpy(body, lx->input + body_start, body_len);
            body[body_len] = '\0';
            word_add_piece(w, PIECE_ARITH, quoted, body);
            free(body);
            lx->pos = end;
            return;
        }
        size_t end = scan_balanced(lx, lx->pos + 1, '(', ')');
        if (lx->incomplete)
            return;
        size_t body_start = lx->pos + 1;
        size_t body_len = end - body_start - 1;
        char *body = xmalloc(body_len + 1);
        memcpy(body, lx->input + body_start, body_len);
        body[body_len] = '\0';
        word_add_piece(w, PIECE_CMDSUB, quoted, body);
        free(body);
        lx->pos = end;
        return;
    }
    if (c == '{') {
        size_t end = scan_balanced(lx, lx->pos + 1, '{', '}');
        if (lx->incomplete)
            return;
        size_t body_start = lx->pos;
        size_t body_len = end - body_start;
        char *body = xmalloc(body_len + 1);
        memcpy(body, lx->input + body_start, body_len);
        body[body_len] = '\0';
        word_add_piece(w, PIECE_PARAM, quoted, body);
        free(body);
        lx->pos = end;
        return;
    }
    if (is_name_start(c)) {
        size_t end = lx->pos;
        while (end < len && is_name_char(lx->input[end]))
            end++;
        char *name = xmalloc(end - lx->pos + 1);
        memcpy(name, lx->input + lx->pos, end - lx->pos);
        name[end - lx->pos] = '\0';
        word_add_piece(w, PIECE_PARAM, quoted, name);
        free(name);
        lx->pos = end;
        return;
    }
    if (strchr("0123456789@*#?$!-", c)) {
        char spec[2] = { c, '\0' };
        word_add_piece(w, PIECE_PARAM, quoted, spec);
        lx->pos++;
        return;
    }
    lx->pos = start;
    word_append_text(w, quoted, "$", 1);
    lx->pos++;
}

static void scan_backtick(Lexer *lx, Word *w, int quoted)
{
    size_t len = strlen(lx->input);
    lx->pos++;
    StrBuf sb;
    sb_init(&sb);
    while (lx->pos < len) {
        char c = lx->input[lx->pos];
        if (c == '\\' && lx->pos + 1 < len &&
            strchr("$`\\", lx->input[lx->pos + 1])) {
            sb_append_char(&sb, lx->input[lx->pos + 1]);
            lx->pos += 2;
            continue;
        }
        if (c == '`') {
            lx->pos++;
            word_add_piece(w, PIECE_CMDSUB, quoted, sb.data ? sb.data : "");
            sb_free(&sb);
            return;
        }
        sb_append_char(&sb, c);
        lx->pos++;
    }
    lx->incomplete = 1;
    sb_free(&sb);
}

static void scan_double_quote(Lexer *lx, Word *w)
{
    size_t len = strlen(lx->input);
    lx->pos++;
    while (lx->pos < len) {
        char c = lx->input[lx->pos];
        if (c == '"') {
            lx->pos++;
            return;
        }
        if (c == '\\') {
            if (lx->pos + 1 >= len) {
                lx->incomplete = 1;
                return;
            }
            char next = lx->input[lx->pos + 1];
            if (strchr("$`\"\\", next)) {
                word_append_text(w, 1, &next, 1);
                lx->pos += 2;
                continue;
            }
            if (next == '\n') {
                lx->pos += 2;
                continue;
            }
            word_append_text(w, 1, "\\", 1);
            lx->pos++;
            continue;
        }
        if (c == '$') {
            scan_dollar(lx, w, 1);
            if (lx->incomplete)
                return;
            continue;
        }
        if (c == '`') {
            scan_backtick(lx, w, 1);
            if (lx->incomplete)
                return;
            continue;
        }
        word_append_text(w, 1, &c, 1);
        lx->pos++;
    }
    lx->incomplete = 1;
}

static void scan_word(Lexer *lx, Word *w)
{
    size_t len = strlen(lx->input);
    while (lx->pos < len) {
        char c = lx->input[lx->pos];
        if (is_blank(c) || c == '\n' || is_operator_start(c))
            break;
        if (c == '\\') {
            lx->pos++;
            if (lx->pos >= len) {
                lx->incomplete = 1;
                return;
            }
            if (lx->input[lx->pos] == '\n') {
                lx->pos++;
                continue;
            }
            word_append_text(w, 1, &lx->input[lx->pos], 1);
            lx->pos++;
            continue;
        }
        if (c == '\'') {
            lx->pos++;
            size_t start = lx->pos;
            while (lx->pos < len && lx->input[lx->pos] != '\'')
                lx->pos++;
            if (lx->pos >= len) {
                lx->incomplete = 1;
                word_append_text(w, 1, lx->input + start, lx->pos - start);
                return;
            }
            int before = w->count;
            word_append_text(w, 1, lx->input + start, lx->pos - start);
            if (w->count == before)
                word_add_piece(w, PIECE_TEXT, 1, "");
            lx->pos++;
            continue;
        }
        if (c == '"') {
            int before = w->count;
            scan_double_quote(lx, w);
            if (lx->incomplete)
                return;
            if (w->count == before)
                word_add_piece(w, PIECE_TEXT, 1, "");
            continue;
        }
        if (c == '$') {
            scan_dollar(lx, w, 0);
            if (lx->incomplete)
                return;
            continue;
        }
        if (c == '`') {
            scan_backtick(lx, w, 0);
            if (lx->incomplete)
                return;
            continue;
        }
        word_append_text(w, 0, &c, 1);
        lx->pos++;
    }
}

static int word_is_all_digits(const Word *w)
{
    if (w->count == 0)
        return 0;
    for (int i = 0; i < w->count; i++) {
        if (w->items[i].kind != PIECE_TEXT || w->items[i].quoted)
            return 0;
        for (const char *p = w->items[i].text; *p; p++)
            if (!isdigit((unsigned char)*p))
                return 0;
    }
    return 1;
}

static void consume_heredocs(Lexer *lx)
{
    size_t len = strlen(lx->input);
    for (int i = 0; i < lx->pending_count; i++) {
        StrBuf sb;
        sb_init(&sb);
        sb_append(&sb, lx->pending[i].body->text);
        for (;;) {
            if (lx->pos >= len) {
                lx->incomplete = 1;
                free(lx->pending[i].body->text);
                lx->pending[i].body->text = sb_take(&sb);
                return;
            }
            size_t eol = lx->pos;
            while (eol < len && lx->input[eol] != '\n')
                eol++;
            const char *line = lx->input + lx->pos;
            size_t line_len = eol - lx->pos;
            const char *cmp = line;
            size_t cmp_len = line_len;
            if (lx->pending[i].strip_tabs) {
                while (cmp_len > 0 && *cmp == '\t') {
                    cmp++;
                    cmp_len--;
                }
            }
            if (cmp_len == strlen(lx->pending[i].delim) &&
                memcmp(cmp, lx->pending[i].delim, cmp_len) == 0) {
                lx->pos = eol < len ? eol + 1 : len;
                break;
            }
            sb_append_n(&sb, line, line_len);
            sb_append_char(&sb, '\n');
            lx->pos = eol < len ? eol + 1 : len;
        }
        free(lx->pending[i].body->text);
        lx->pending[i].body->text = sb_take(&sb);
        if (lx->pending[i].strip_tabs) {
            char *text = lx->pending[i].body->text;
            char *r = text;
            char *w = text;
            int line_start = 1;
            while (*r) {
                if (line_start && *r == '\t') {
                    r++;
                    continue;
                }
                line_start = *r == '\n';
                *w++ = *r++;
            }
            *w = '\0';
        }
        free(lx->pending[i].delim);
    }
    lx->pending_count = 0;
}

static int read_redir(Lexer *lx, Token *tok, int fd)
{
    size_t len = strlen(lx->input);
    char c = lx->input[lx->pos];
    tok->kind = TOK_REDIR;
    tok->fd = fd;
    tok->heredoc = NULL;
    if (c == '<') {
        lx->pos++;
        if (lx->pos < len && lx->input[lx->pos] == '<') {
            lx->pos++;
            if (lx->pos < len && lx->input[lx->pos] == '<') {
                lx->pos++;
                tok->redir_kind = REDIR_HERESTRING;
                if (fd < 0)
                    tok->fd = 0;
                return 0;
            }
            int strip_tabs = 0;
            if (lx->pos < len && lx->input[lx->pos] == '-') {
                strip_tabs = 1;
                lx->pos++;
            }
            tok->redir_kind = REDIR_HEREDOC;
            if (fd < 0)
                tok->fd = 0;
            while (lx->pos < len && is_blank(lx->input[lx->pos]))
                lx->pos++;
            Word delim_word;
            word_init(&delim_word);
            scan_word(lx, &delim_word);
            if (delim_word.count == 0 || lx->incomplete) {
                word_free(&delim_word);
                if (!lx->incomplete)
                    snprintf(lx->error, sizeof(lx->error),
                             "syntax error near unexpected token `newline'");
                return -1;
            }
            int quoted = 0;
            for (int i = 0; i < delim_word.count; i++)
                if (delim_word.items[i].quoted)
                    quoted = 1;
            HeredocBody *body = heredoc_body_new(lx, quoted);
            tok->heredoc = body;
            if (lx->pending_count < 32) {
                lx->pending[lx->pending_count].delim =
                    word_raw_text(&delim_word);
                lx->pending[lx->pending_count].strip_tabs = strip_tabs;
                lx->pending[lx->pending_count].body = body;
                lx->pending_count++;
            }
            word_free(&delim_word);
            return 0;
        }
        if (lx->pos < len && lx->input[lx->pos] == '&') {
            lx->pos++;
            tok->redir_kind = REDIR_DUP_IN;
        } else if (lx->pos < len && lx->input[lx->pos] == '>') {
            lx->pos++;
            tok->redir_kind = REDIR_RDWR;
        } else {
            tok->redir_kind = REDIR_INPUT;
        }
        if (fd < 0)
            tok->fd = 0;
        return 0;
    }
    lx->pos++;
    if (lx->pos < len && lx->input[lx->pos] == '>') {
        lx->pos++;
        tok->redir_kind = REDIR_APPEND;
    } else if (lx->pos < len && lx->input[lx->pos] == '|') {
        lx->pos++;
        tok->redir_kind = REDIR_CLOBBER;
    } else if (lx->pos < len && lx->input[lx->pos] == '&') {
        lx->pos++;
        tok->redir_kind = REDIR_DUP_OUT;
    } else {
        tok->redir_kind = REDIR_OUTPUT;
    }
    if (fd < 0)
        tok->fd = 1;
    return 0;
}

int lexer_next(Lexer *lx, Token *tok)
{
    size_t len = strlen(lx->input);
    memset(tok, 0, sizeof(*tok));
    word_init(&tok->word);
    tok->fd = -1;

    while (lx->pos < len && is_blank(lx->input[lx->pos]))
        lx->pos++;
    if (lx->pos >= len) {
        tok->kind = TOK_EOF;
        if (lx->pending_count > 0) {
            lx->incomplete = 1;
            return -1;
        }
        return 0;
    }
    char c = lx->input[lx->pos];
    if (c == '\n') {
        lx->pos++;
        consume_heredocs(lx);
        tok->kind = TOK_NEWLINE;
        return lx->incomplete ? -1 : 0;
    }
    if (c == '#' && (options.interactive_comments || !shell.interactive)) {
        while (lx->pos < len && lx->input[lx->pos] != '\n')
            lx->pos++;
        return lexer_next(lx, tok);
    }
    if (c == '|') {
        lx->pos++;
        if (lx->pos < len && lx->input[lx->pos] == '|') {
            lx->pos++;
            tok->kind = TOK_OR_IF;
        } else {
            tok->kind = TOK_PIPE;
        }
        return 0;
    }
    if (c == '&') {
        if (lx->pos + 1 < len && lx->input[lx->pos + 1] == '&') {
            lx->pos += 2;
            tok->kind = TOK_AND_IF;
            return 0;
        }
        if (lx->pos + 1 < len && lx->input[lx->pos + 1] == '>') {
            lx->pos += 2;
            tok->kind = TOK_REDIR;
            tok->fd = 1;
            if (lx->pos < len && lx->input[lx->pos] == '>') {
                lx->pos++;
                tok->redir_kind = REDIR_ALL_APPEND;
            } else {
                tok->redir_kind = REDIR_ALL_OUT;
            }
            return 0;
        }
        lx->pos++;
        tok->kind = TOK_AMP;
        return 0;
    }
    if (c == ';') {
        lx->pos++;
        if (lx->pos < len && lx->input[lx->pos] == ';') {
            lx->pos++;
            tok->kind = TOK_DSEMI;
        } else {
            tok->kind = TOK_SEMI;
        }
        return 0;
    }
    if (c == '(') {
        lx->pos++;
        tok->kind = TOK_LPAREN;
        return 0;
    }
    if (c == ')') {
        lx->pos++;
        tok->kind = TOK_RPAREN;
        return 0;
    }
    if (c == '<' || c == '>')
        return read_redir(lx, tok, -1);

    scan_word(lx, &tok->word);
    if (lx->incomplete) {
        tok->kind = TOK_EOF;
        return -1;
    }
    if (tok->word.count == 0) {
        snprintf(lx->error, sizeof(lx->error),
                 "syntax error near unexpected token `%c'", c);
        return -1;
    }
    if (word_is_all_digits(&tok->word) && lx->pos < len &&
        (lx->input[lx->pos] == '<' || lx->input[lx->pos] == '>')) {
        char *raw = word_raw_text(&tok->word);
        int fd = atoi(raw);
        free(raw);
        word_free(&tok->word);
        return read_redir(lx, tok, fd);
    }
    tok->kind = TOK_WORD;
    return 0;
}

int lex_single_word(const char *text, Word *out)
{
    Lexer lx;
    lexer_init(&lx, text);
    Token tok;
    int rc = lexer_next(&lx, &tok);
    if (rc == 0 && tok.kind == TOK_WORD) {
        *out = tok.word;
        word_init(&tok.word);
        lexer_free(&lx);
        return 0;
    }
    word_free(&tok.word);
    lexer_free(&lx);
    word_init(out);
    return -1;
}

void lex_heredoc_body(const char *body, Word *out)
{
    size_t len = strlen(body);
    size_t pos = 0;
    word_init(out);
    Lexer lx;
    lexer_init(&lx, body);
    while (pos < len) {
        char c = body[pos];
        if (c == '\\' && pos + 1 < len && strchr("$`\\\n", body[pos + 1])) {
            if (body[pos + 1] != '\n')
                word_append_text(out, 1, &body[pos + 1], 1);
            pos += 2;
            continue;
        }
        if (c == '$') {
            lx.pos = pos;
            scan_dollar(&lx, out, 1);
            if (lx.incomplete) {
                word_append_text(out, 1, body + pos, len - pos);
                pos = len;
                break;
            }
            pos = lx.pos;
            continue;
        }
        if (c == '`') {
            lx.pos = pos;
            scan_backtick(&lx, out, 1);
            if (lx.incomplete) {
                word_append_text(out, 1, body + pos, len - pos);
                pos = len;
                break;
            }
            pos = lx.pos;
            continue;
        }
        word_append_text(out, 1, &c, 1);
        pos++;
    }
    lexer_free(&lx);
}
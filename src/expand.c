#include "ash.h"
#include <ctype.h>
#include <dirent.h>
#include <fnmatch.h>
#include <glob.h>
#include <limits.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

static char *llstr(long long v)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", v);
    return xstrdup(buf);
}

static int is_name_start(int c)
{
    return isalpha(c) || c == '_';
}

static int is_name_char(int c)
{
    return isalnum(c) || c == '_';
}

static char *get_param_value(const char *name)
{
    if (strcmp(name, "?") == 0)
        return llstr(shell.last_status);
    if (strcmp(name, "$") == 0)
        return llstr(shell.shell_pid);
    if (strcmp(name, "0") == 0)
        return xstrdup(shell.shell_name ? shell.shell_name : "ash");
    if (strcmp(name, "#") == 0)
        return llstr(shell.positional_count);
    if (strcmp(name, "!") == 0)
        return shell.last_bg_pid ? llstr(shell.last_bg_pid) : xstrdup("");
    if (strcmp(name, "-") == 0) {
        StrBuf sb;
        sb_init(&sb);
        if (options.errexit)
            sb_append_char(&sb, 'e');
        if (options.nounset)
            sb_append_char(&sb, 'u');
        if (options.xtrace)
            sb_append_char(&sb, 'x');
        if (shell.interactive)
            sb_append_char(&sb, 'i');
        return sb_take(&sb);
    }
    if (strcmp(name, "@") == 0 || strcmp(name, "*") == 0) {
        StrBuf sb;
        sb_init(&sb);
        for (int i = 0; i < shell.positional_count; i++) {
            if (i > 0)
                sb_append_char(&sb, ' ');
            sb_append(&sb, shell.positional[i]);
        }
        return sb_take(&sb);
    }
    if (isdigit((unsigned char)name[0]) && name[1] == '\0') {
        int idx = name[0] - '0';
        if (idx >= 1 && idx <= shell.positional_count)
            return xstrdup(shell.positional[idx - 1]);
        return xstrdup("");
    }
    const char *v = var_get(name);
    return xstrdup(v ? v : "");
}

static int is_regular_name(const char *s)
{
    if (!isalpha((unsigned char)s[0]) && s[0] != '_')
        return 0;
    for (const char *p = s + 1; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '_')
            return 0;
    return 1;
}

static void expand_fatal(const char *msg)
{
    fprintf(stderr, "ash: %s\n", msg);
    if (!shell.interactive) {
        shell.exit_pending = 1;
        shell.exit_status = 1;
    }
}

static char *trim_pattern(const char *val, const char *pat, int prefix,
                          int longest)
{
    size_t n = strlen(val);
    char *tmp = xmalloc(n + 1);
    if (prefix) {
        if (longest) {
            for (size_t i = n + 1; i > 0; i--) {
                memcpy(tmp, val, i - 1);
                tmp[i - 1] = '\0';
                if (fnmatch(pat, tmp, 0) == 0) {
                    free(tmp);
                    return xstrdup(val + i - 1);
                }
            }
        } else {
            for (size_t i = 0; i <= n; i++) {
                memcpy(tmp, val, i);
                tmp[i] = '\0';
                if (fnmatch(pat, tmp, 0) == 0) {
                    free(tmp);
                    return xstrdup(val + i);
                }
            }
        }
    } else {
        if (longest) {
            for (size_t i = 0; i <= n; i++) {
                if (fnmatch(pat, val + i, 0) == 0) {
                    free(tmp);
                    char *out = xmalloc(i + 1);
                    memcpy(out, val, i);
                    out[i] = '\0';
                    return out;
                }
            }
        } else {
            for (size_t i = n + 1; i > 0; i--) {
                if (fnmatch(pat, val + i - 1, 0) == 0) {
                    free(tmp);
                    char *out = xmalloc(i);
                    memcpy(out, val, i - 1);
                    out[i - 1] = '\0';
                    return out;
                }
            }
        }
    }
    free(tmp);
    return xstrdup(val);
}

static char *param_expand(const char *spec, int *error)
{
    if (spec[0] != '{') {
        if (options.nounset && is_regular_name(spec) && !var_get(spec)) {
            char msg[256];
            snprintf(msg, sizeof(msg), "%s: parameter not set", spec);
            expand_fatal(msg);
            *error = 1;
            return NULL;
        }
        return get_param_value(spec);
    }
    size_t len = strlen(spec);
    if (len < 2 || spec[len - 1] != '}') {
        expand_fatal("bad substitution");
        *error = 1;
        return NULL;
    }
    char *inner = xstrdup(spec + 1);
    inner[len - 2] = '\0';
    if (inner[0] == '#' && inner[1] != '\0') {
        char *val = get_param_value(inner + 1);
        char *out = llstr((long long)strlen(val));
        free(val);
        free(inner);
        return out;
    }
    char *name = NULL;
    char *rest = NULL;
    if (is_name_start(inner[0])) {
        size_t end = 0;
        while (inner[end] && is_name_char(inner[end]))
            end++;
        name = xmalloc(end + 1);
        memcpy(name, inner, end);
        name[end] = '\0';
        rest = inner + end;
    } else if (inner[0] && strchr("@*#?$!-0123456789", inner[0])) {
        name = xmalloc(2);
        name[0] = inner[0];
        name[1] = '\0';
        rest = inner + 1;
    } else {
        char msg[256];
        snprintf(msg, sizeof(msg), "%s: bad substitution", spec);
        expand_fatal(msg);
        *error = 1;
        free(inner);
        return NULL;
    }
    char *out = NULL;
    if (*rest == '\0') {
        if (options.nounset && is_regular_name(name) && !var_get(name)) {
            char msg[256];
            snprintf(msg, sizeof(msg), "%s: parameter not set", name);
            expand_fatal(msg);
            *error = 1;
        } else {
            out = get_param_value(name);
        }
        free(name);
        free(inner);
        return out;
    }
    int colon = 0;
    char *op = rest;
    if (rest[0] == ':' && rest[1] && strchr("-=+?", rest[1])) {
        colon = 1;
        op = rest + 1;
    }
    if (op[0] == '-' || op[0] == '=' || op[0] == '+' || op[0] == '?') {
        Word w;
        word_init(&w);
        lex_single_word(op + 1, &w);
        char *wordval = expand_word_string(&w, EXP_NOSPLIT | EXP_NOGLOB);
        word_free(&w);
        if (!wordval)
            wordval = xstrdup("");
        int is_unset;
        char *val;
        if (is_regular_name(name)) {
            const char *v = var_get(name);
            is_unset = !v;
            val = xstrdup(v ? v : "");
        } else {
            val = get_param_value(name);
            is_unset = isdigit((unsigned char)name[0]) &&
                       (name[0] - '0') > shell.positional_count;
        }
        int use_alt = colon ? (is_unset || val[0] == '\0') : is_unset;
        if (op[0] == '-') {
            out = use_alt ? xstrdup(wordval) : xstrdup(val);
        } else if (op[0] == '=') {
            if (use_alt) {
                if (is_regular_name(name))
                    var_set(name, wordval, 0);
                out = xstrdup(wordval);
            } else {
                out = xstrdup(val);
            }
        } else if (op[0] == '+') {
            out = use_alt ? xstrdup("") : xstrdup(wordval);
        } else {
            if (use_alt) {
                char msg[512];
                snprintf(msg, sizeof(msg), "%s: %s", name,
                         wordval[0] ? wordval : "parameter not set");
                expand_fatal(msg);
                *error = 1;
            } else {
                out = xstrdup(val);
            }
        }
        free(val);
        free(wordval);
    } else if (op[0] == '#' || op[0] == '%') {
        int longest = op[1] == op[0];
        char *pattext = op + (longest ? 2 : 1);
        Word w;
        word_init(&w);
        lex_single_word(pattext, &w);
        char *pat = expand_word_string(&w, EXP_NOSPLIT | EXP_NOGLOB);
        word_free(&w);
        if (!pat)
            pat = xstrdup("");
        char *val = get_param_value(name);
        out = trim_pattern(val, pat, op[0] == '#', longest);
        free(val);
        free(pat);
    } else {
        char msg[256];
        snprintf(msg, sizeof(msg), "%s: bad substitution", spec);
        expand_fatal(msg);
        *error = 1;
    }
    free(name);
    free(inner);
    return out;
}

typedef struct {
    const char *s;
    size_t pos;
    int error;
} ArithParser;

static void a_skip(ArithParser *p)
{
    while (isspace((unsigned char)p->s[p->pos]))
        p->pos++;
}

static long long a_expr(ArithParser *p);

static long long a_primary(ArithParser *p)
{
    a_skip(p);
    char c = p->s[p->pos];
    if (c == '(') {
        p->pos++;
        long long v = a_expr(p);
        a_skip(p);
        if (p->s[p->pos] == ')')
            p->pos++;
        else
            p->error = 1;
        return v;
    }
    if (isdigit((unsigned char)c)) {
        char *end;
        long long v = strtoll(p->s + p->pos, &end, 0);
        if (end == p->s + p->pos) {
            p->error = 1;
            return 0;
        }
        p->pos = end - p->s;
        return v;
    }
    if (isalpha((unsigned char)c) || c == '_') {
        size_t start = p->pos;
        while (isalnum((unsigned char)p->s[p->pos]) || p->s[p->pos] == '_')
            p->pos++;
        char *name = xmalloc(p->pos - start + 1);
        memcpy(name, p->s + start, p->pos - start);
        name[p->pos - start] = '\0';
        const char *val = var_get(name);
        long long v = 0;
        if (val && val[0]) {
            int err = 0;
            v = arith_eval(val, &err);
            if (err)
                p->error = 1;
        }
        free(name);
        return v;
    }
    p->error = 1;
    return 0;
}

static long long a_unary(ArithParser *p)
{
    a_skip(p);
    char c = p->s[p->pos];
    if (c == '!') {
        p->pos++;
        return !a_unary(p);
    }
    if (c == '~') {
        p->pos++;
        return ~a_unary(p);
    }
    if (c == '-') {
        p->pos++;
        return -a_unary(p);
    }
    if (c == '+') {
        p->pos++;
        return a_unary(p);
    }
    return a_primary(p);
}

static long long a_mul(ArithParser *p)
{
    long long v = a_unary(p);
    for (;;) {
        a_skip(p);
        char c = p->s[p->pos];
        if (c == '*') {
            p->pos++;
            v *= a_unary(p);
        } else if (c == '/' || c == '%') {
            p->pos++;
            long long r = a_unary(p);
            if (r == 0) {
                p->error = 1;
                return 0;
            }
            v = c == '/' ? v / r : v % r;
        } else {
            return v;
        }
    }
}

static long long a_add(ArithParser *p)
{
    long long v = a_mul(p);
    for (;;) {
        a_skip(p);
        char c = p->s[p->pos];
        if (c == '+') {
            p->pos++;
            v += a_mul(p);
        } else if (c == '-') {
            p->pos++;
            v -= a_mul(p);
        } else {
            return v;
        }
    }
}

static long long a_shift(ArithParser *p)
{
    long long v = a_add(p);
    for (;;) {
        a_skip(p);
        if (p->s[p->pos] == '<' && p->s[p->pos + 1] == '<') {
            p->pos += 2;
            v <<= a_add(p);
        } else if (p->s[p->pos] == '>' && p->s[p->pos + 1] == '>') {
            p->pos += 2;
            v >>= a_add(p);
        } else {
            return v;
        }
    }
}

static long long a_rel(ArithParser *p)
{
    long long v = a_shift(p);
    for (;;) {
        a_skip(p);
        char c = p->s[p->pos];
        char d = p->s[p->pos + 1];
        if (c == '<' && d == '=') {
            p->pos += 2;
            v = v <= a_shift(p);
        } else if (c == '>' && d == '=') {
            p->pos += 2;
            v = v >= a_shift(p);
        } else if (c == '<' && d != '<') {
            p->pos++;
            v = v < a_shift(p);
        } else if (c == '>' && d != '>') {
            p->pos++;
            v = v > a_shift(p);
        } else {
            return v;
        }
    }
}

static long long a_eq(ArithParser *p)
{
    long long v = a_rel(p);
    for (;;) {
        a_skip(p);
        if (p->s[p->pos] == '=' && p->s[p->pos + 1] == '=') {
            p->pos += 2;
            v = v == a_rel(p);
        } else if (p->s[p->pos] == '!' && p->s[p->pos + 1] == '=') {
            p->pos += 2;
            v = v != a_rel(p);
        } else {
            return v;
        }
    }
}

static long long a_band(ArithParser *p)
{
    long long v = a_eq(p);
    for (;;) {
        a_skip(p);
        if (p->s[p->pos] == '&' && p->s[p->pos + 1] != '&') {
            p->pos++;
            v &= a_eq(p);
        } else {
            return v;
        }
    }
}

static long long a_bxor(ArithParser *p)
{
    long long v = a_band(p);
    for (;;) {
        a_skip(p);
        if (p->s[p->pos] == '^') {
            p->pos++;
            v ^= a_band(p);
        } else {
            return v;
        }
    }
}

static long long a_bor(ArithParser *p)
{
    long long v = a_bxor(p);
    for (;;) {
        a_skip(p);
        if (p->s[p->pos] == '|' && p->s[p->pos + 1] != '|') {
            p->pos++;
            v |= a_bxor(p);
        } else {
            return v;
        }
    }
}

static long long a_land(ArithParser *p)
{
    long long v = a_bor(p);
    for (;;) {
        a_skip(p);
        if (p->s[p->pos] == '&' && p->s[p->pos + 1] == '&') {
            p->pos += 2;
            long long r = a_bor(p);
            v = v && r;
        } else {
            return v;
        }
    }
}

static long long a_expr(ArithParser *p)
{
    long long v = a_land(p);
    for (;;) {
        a_skip(p);
        if (p->s[p->pos] == '|' && p->s[p->pos + 1] == '|') {
            p->pos += 2;
            long long r = a_land(p);
            v = v || r;
        } else {
            return v;
        }
    }
}

long long arith_eval(const char *expr, int *error)
{
    ArithParser p = { expr, 0, 0 };
    long long v = a_expr(&p);
    a_skip(&p);
    if (p.s[p.pos] != '\0')
        p.error = 1;
    *error = p.error;
    return v;
}

static char *tilde_expand(const char *text, int assign_context)
{
    StrBuf out;
    sb_init(&out);
    size_t len = strlen(text);
    size_t i = 0;
    int at_start = 1;
    while (i < len) {
        if (text[i] == '~' && at_start) {
            size_t end = i + 1;
            while (end < len && text[end] != '/' && text[end] != ':')
                end++;
            char *name = xmalloc(end - i);
            memcpy(name, text + i + 1, end - i - 1);
            name[end - i - 1] = '\0';
            const char *home = NULL;
            if (name[0] == '\0') {
                home = var_get("HOME");
            } else {
                struct passwd *pw = getpwnam(name);
                if (pw)
                    home = pw->pw_dir;
            }
            if (home)
                sb_append(&out, home);
            else
                sb_append_n(&out, text + i, end - i);
            free(name);
            i = end;
            at_start = 0;
            continue;
        }
        at_start = assign_context && text[i] == ':';
        sb_append_char(&out, text[i]);
        i++;
    }
    return sb_take(&out);
}

static char *eval_piece(const Piece *p, int *error)
{
    if (p->kind == PIECE_TEXT)
        return xstrdup(p->text);
    if (p->kind == PIECE_PARAM)
        return param_expand(p->text, error);
    if (p->kind == PIECE_CMDSUB) {
        char *out = NULL;
        exec_command_string_capture(p->text, &out);
        return out ? out : xstrdup("");
    }
    int err = 0;
    long long v = arith_eval(p->text, &err);
    if (err) {
        expand_fatal("arithmetic syntax error");
        *error = 1;
        return NULL;
    }
    return llstr(v);
}

char *expand_word_string(const Word *w, int flags)
{
    StrBuf sb;
    sb_init(&sb);
    for (int i = 0; i < w->count; i++) {
        const Piece *p = &w->items[i];
        if (p->kind == PIECE_TEXT) {
            if (!p->quoted && i == 0) {
                char *t = tilde_expand(p->text, flags & EXP_ASSIGN);
                sb_append(&sb, t);
                free(t);
            } else {
                sb_append(&sb, p->text);
            }
            continue;
        }
        int error = 0;
        char *val = eval_piece(p, &error);
        if (error || !val) {
            free(val);
            sb_free(&sb);
            return NULL;
        }
        sb_append(&sb, val);
        free(val);
    }
    return sb_take(&sb);
}

typedef struct {
    char **items;
    int count;
    int cap;
} FieldList;

static void fl_add(FieldList *fl, char *s)
{
    if (fl->count == fl->cap) {
        fl->cap = fl->cap ? fl->cap * 2 : 8;
        fl->items = xrealloc(fl->items, fl->cap * sizeof(char *));
    }
    fl->items[fl->count++] = s;
}

static void append_escaped(StrBuf *sb, const char *text)
{
    for (const char *p = text; *p; p++) {
        if (strchr("*?[\\", *p))
            sb_append_char(sb, '\\');
        sb_append_char(sb, *p);
    }
}

static char *unescape_glob(const char *text)
{
    StrBuf sb;
    sb_init(&sb);
    for (const char *p = text; *p; p++) {
        if (*p == '\\' && p[1] && strchr("*?[\\", p[1])) {
            p++;
            sb_append_char(&sb, *p);
            continue;
        }
        sb_append_char(&sb, *p);
    }
    return sb_take(&sb);
}

static int str_cmp(const void *a, const void *b)
{
    return strcmp(*(const char **)a, *(const char **)b);
}

static void glob_walk(const char *dir, char **segs, int nseg, int seg_idx,
                      const char *prefix, FieldList *out)
{
    if (seg_idx == nseg) {
        if (prefix[0])
            fl_add(out, xstrdup(prefix));
        return;
    }
    const char *seg = segs[seg_idx];
    DIR *d = opendir(dir[0] ? dir : ".");
    if (!d)
        return;
    int is_doublestar = strcmp(seg, "**") == 0;
    if (is_doublestar)
        glob_walk(dir, segs, nseg, seg_idx + 1, prefix, out);
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        if (ent->d_name[0] == '.' && seg[0] != '.')
            continue;
        if (!is_doublestar && fnmatch(seg, ent->d_name, 0) != 0)
            continue;
        StrBuf newdir;
        sb_init(&newdir);
        sb_append(&newdir, dir);
        if (newdir.len > 0 && newdir.data[newdir.len - 1] != '/')
            sb_append_char(&newdir, '/');
        sb_append(&newdir, ent->d_name);
        StrBuf newprefix;
        sb_init(&newprefix);
        sb_append(&newprefix, prefix);
        sb_append(&newprefix, ent->d_name);
        if (is_doublestar) {
            struct stat st;
            if (stat(newdir.data, &st) == 0 && S_ISDIR(st.st_mode)) {
                sb_append_char(&newprefix, '/');
                glob_walk(newdir.data, segs, nseg, seg_idx, newprefix.data, out);
            } else if (seg_idx == nseg - 1) {
                fl_add(out, sb_take(&newprefix));
            }
        } else if (seg_idx == nseg - 1) {
            fl_add(out, sb_take(&newprefix));
        } else {
            struct stat st;
            if (stat(newdir.data, &st) == 0 && S_ISDIR(st.st_mode)) {
                sb_append_char(&newprefix, '/');
                glob_walk(newdir.data, segs, nseg, seg_idx + 1,
                          newprefix.data, out);
            }
        }
        sb_free(&newdir);
        sb_free(&newprefix);
    }
    closedir(d);
}

static void glob_recursive(const char *pattern, FieldList *out)
{
    char *copy = xstrdup(pattern);
    char *segs[128];
    int nseg = 0;
    const char *start = copy;
    char prefix[2] = "";
    if (copy[0] == '/') {
        prefix[0] = '/';
        prefix[1] = '\0';
        start = copy + 1;
    }
    char *save = NULL;
    for (char *tok = strtok_r((char *)start, "/", &save); tok && nseg < 128;
         tok = strtok_r(NULL, "/", &save))
        segs[nseg++] = tok;
    glob_walk(prefix[0] ? "/" : "", segs, nseg, 0, prefix, out);
    free(copy);
    if (out->count > 0 && pattern[strlen(pattern) - 1] != '/') {
        for (int i = 0; i < out->count; i++) {
            size_t n = strlen(out->items[i]);
            if (n > 1 && out->items[i][n - 1] == '/')
                out->items[i][n - 1] = '\0';
        }
    }
    if (out->count > 1) {
        qsort(out->items, out->count, sizeof(char *), str_cmp);
        int w = 1;
        for (int i = 1; i < out->count; i++) {
            if (strcmp(out->items[i], out->items[w - 1]) == 0)
                free(out->items[i]);
            else
                out->items[w++] = out->items[i];
        }
        out->count = w;
    }
}

static void glob_field(const char *pattern, FieldList *out)
{
    int before = out->count;
    if (strstr(pattern, "**")) {
        glob_recursive(pattern, out);
        if (out->count > before)
            return;
    } else {
        glob_t gl;
        int rc = glob(pattern, 0, NULL, &gl);
        if (rc == 0) {
            for (size_t i = 0; i < gl.gl_pathc; i++)
                fl_add(out, xstrdup(gl.gl_pathv[i]));
            globfree(&gl);
            return;
        }
        globfree(&gl);
    }
    fl_add(out, unescape_glob(pattern));
}

static int expand_one_fields(const Word *w, int flags, FieldList *out)
{
    StrBuf cur;
    sb_init(&cur);
    int has_content = 0;
    int globbable = 0;
    FieldList raw;
    memset(&raw, 0, sizeof(raw));
    int failed = 0;

    for (int i = 0; i < w->count && !failed; i++) {
        const Piece *p = &w->items[i];
        if (p->kind == PIECE_TEXT) {
            if (p->quoted) {
                append_escaped(&cur, p->text);
                has_content = 1;
            } else {
                char *t = i == 0 ? tilde_expand(p->text, flags & EXP_ASSIGN)
                                 : xstrdup(p->text);
                sb_append(&cur, t);
                if (t[0])
                    has_content = 1;
                if (strpbrk(t, "*?["))
                    globbable = 1;
                free(t);
            }
            continue;
        }
        int error = 0;
        char *val = eval_piece(p, &error);
        if (error || !val) {
            free(val);
            failed = 1;
            break;
        }
        if (p->quoted) {
            append_escaped(&cur, val);
            has_content = 1;
        } else {
            const char *ifs = var_get("IFS");
            if (!ifs)
                ifs = " \t\n";
            for (const char *c = val; *c; c++) {
                if (*c != '\0' && strchr(ifs, *c)) {
                    if (cur.len > 0 || has_content) {
                        char *field = xstrdup(cur.data ? cur.data : "");
                        fl_add(&raw, field);
                        if (globbable)
                            fl_add(&raw, xstrdup("\x01GLOB"));
                    }
                    sb_free(&cur);
                    sb_init(&cur);
                    has_content = 0;
                    globbable = 0;
                } else {
                    sb_append_char(&cur, *c);
                    if (strchr("*?[", *c))
                        globbable = 1;
                }
            }
        }
        free(val);
    }
    if (!failed && (cur.len > 0 || has_content)) {
        fl_add(&raw, xstrdup(cur.data ? cur.data : ""));
        if (globbable)
            fl_add(&raw, xstrdup("\x01GLOB"));
    }
    sb_free(&cur);
    if (failed) {
        for (int i = 0; i < raw.count; i++)
            free(raw.items[i]);
        free(raw.items);
        return -1;
    }
    for (int i = 0; i < raw.count; i++) {
        if (strcmp(raw.items[i], "\x01GLOB") == 0) {
            free(raw.items[i]);
            continue;
        }
        int marked = i + 1 < raw.count &&
                     strcmp(raw.items[i + 1], "\x01GLOB") == 0;
        if (marked && !(flags & EXP_NOGLOB) && strpbrk(raw.items[i], "*?[")) {
            glob_field(raw.items[i], out);
        } else {
            fl_add(out, unescape_glob(raw.items[i]));
        }
        free(raw.items[i]);
    }
    free(raw.items);
    return 0;
}

#define QUOTE_MARK '\x01'

static void flatten_word(const Word *w, StrBuf *sb)
{
    for (int i = 0; i < w->count; i++) {
        const Piece *p = &w->items[i];
        if (p->kind == PIECE_TEXT) {
            if (p->quoted) {
                sb_append_char(sb, QUOTE_MARK);
                sb_append(sb, p->text);
                sb_append_char(sb, QUOTE_MARK);
            } else {
                sb_append(sb, p->text);
            }
            continue;
        }
        if (p->quoted)
            sb_append_char(sb, QUOTE_MARK);
        sb_append_char(sb, '$');
        if (p->kind == PIECE_PARAM) {
            sb_append(sb, p->text);
        } else if (p->kind == PIECE_CMDSUB) {
            sb_append_char(sb, '(');
            sb_append(sb, p->text);
            sb_append_char(sb, ')');
        } else {
            sb_append(sb, "((");
            sb_append(sb, p->text);
            sb_append(sb, "))");
        }
        if (p->quoted)
            sb_append_char(sb, QUOTE_MARK);
    }
}

/*@ requires i < len;
    requires \valid_read(s + (0 .. len - 1));
    assigns \nothing;
    ensures \old(i) < \result <= len;
*/
static size_t flat_skip_marker(const char *s, size_t i, size_t len)
{
    i++;
    /*@ loop invariant i <= len;
        loop invariant \at(i, Pre) < i;
        loop assigns i;
        loop variant len - i;
    */
    while (i < len && s[i] != QUOTE_MARK)
        i++;
    return i < len ? i + 1 : len;
}

/*@ requires i < len;
    requires \valid_read(s + (0 .. len - 1));
    assigns \nothing;
    ensures \old(i) < \result <= len;
*/
static size_t flat_skip_single(const char *s, size_t i, size_t len)
{
    i++;
    /*@ loop invariant i <= len;
        loop invariant \at(i, Pre) < i;
        loop assigns i;
        loop variant len - i;
    */
    while (i < len && s[i] != '\'')
        i++;
    return i < len ? i + 1 : len;
}

/*@ requires i < len;
    requires \valid_read(s + (0 .. len - 1));
    assigns \nothing;
    ensures \old(i) < \result <= len;
*/
static size_t flat_skip_double(const char *s, size_t i, size_t len)
{
    i++;
    /*@ loop invariant i <= len;
        loop invariant \at(i, Pre) < i;
        loop assigns i;
        loop variant len - i;
    */
    while (i < len && s[i] != '"') {
        if (s[i] == '\\') {
            if (i + 1 < len)
                i += 2;
            else
                i = len;
        } else {
            i++;
        }
    }
    return i < len ? i + 1 : len;
}

/*@ requires i < len;
    requires \valid_read(s + (0 .. len - 1));
    assigns \nothing;
    ensures \old(i) < \result <= len;
    ensures \result == len || s[\result - 1] == close;
*/
static size_t flat_balanced(const char *s, size_t i, size_t len, char open,
                            char close)
{
    size_t depth = 1;
    i++;
    /*@ loop invariant i <= len;
        loop invariant \at(i, Pre) < i;
        loop invariant depth >= 1;
        loop invariant depth <= i;
        loop assigns i, depth;
        loop variant len - i;
    */
    while (i < len) {
        char c = s[i];
        if (c == QUOTE_MARK) {
            i = flat_skip_marker(s, i, len);
            continue;
        }
        if (c == '\'') {
            i = flat_skip_single(s, i, len);
            continue;
        }
        if (c == '"') {
            i = flat_skip_double(s, i, len);
            continue;
        }
        if (c == '\\') {
            if (i + 1 < len)
                i += 2;
            else
                i = len;
            continue;
        }
        if (c == open)
            depth++;
        else if (c == close) {
            depth--;
            if (depth == 0)
                return i + 1;
        }
        i++;
    }
    return len;
}

/*@ requires i < len;
    requires \valid_read(s + (0 .. len - 1));
    assigns \nothing;
    ensures \old(i) < \result <= len;
*/
static size_t flat_skip(const char *s, size_t i, size_t len)
{
    char c = s[i];
    if (c == QUOTE_MARK)
        return flat_skip_marker(s, i, len);
    if (c == '\'')
        return flat_skip_single(s, i, len);
    if (c == '"')
        return flat_skip_double(s, i, len);
    if (c == '\\')
        return i + 1 < len ? i + 2 : len;
    if (c == '$' && i + 1 < len) {
        if (s[i + 1] == '{')
            return flat_balanced(s, i + 1, len, '{', '}');
        if (s[i + 1] == '(')
            return flat_balanced(s, i + 1, len, '(', ')');
    }
    return i + 1;
}

static int flat_is_range(const char *content)
{
    if (strchr(content, QUOTE_MARK) || strchr(content, '$') ||
        strchr(content, ','))
        return 0;
    char *copy = xstrdup(content);
    char *dots = strstr(copy, "..");
    int is_range = 0;
    if (dots) {
        char *a = copy;
        *dots = '\0';
        char *b = dots + 2;
        char *step = strstr(b, "..");
        if (step)
            *step = '\0';
        char *endp;
        strtoll(a, &endp, 10);
        int ok_a = *a && *endp == '\0';
        strtoll(b, &endp, 10);
        int ok_b = *b && *endp == '\0';
        int ok_c = 1;
        if (step) {
            strtoll(step + 2, &endp, 10);
            ok_c = step[2] && *endp == '\0';
        }
        is_range = ok_a && ok_b && ok_c;
    }
    free(copy);
    return is_range;
}

static int brace_find_group_flat(const char *s, size_t len, size_t *start,
                                 size_t *end)
{
    size_t i = 0;
    while (i < len) {
        if (s[i] == '{') {
            int depth = 1;
            int has_comma = 0;
            size_t j = i + 1;
            while (j < len) {
                char c = s[j];
                if (c == '{') {
                    depth++;
                    j++;
                    continue;
                }
                if (c == '}') {
                    depth--;
                    if (depth == 0)
                        break;
                    j++;
                    continue;
                }
                if (c == ',' && depth == 1)
                    has_comma = 1;
                j = flat_skip(s, j, len);
            }
            if (depth == 0) {
                if (has_comma) {
                    *start = i;
                    *end = j;
                    return 1;
                }
                char *content = xmalloc(j - i);
                memcpy(content, s + i + 1, j - i - 1);
                content[j - i - 1] = '\0';
                int is_range = flat_is_range(content);
                free(content);
                if (is_range) {
                    *start = i;
                    *end = j;
                    return 1;
                }
            }
        }
        i = flat_skip(s, i, len);
    }
    return 0;
}

static void unflatten_to_word(const char *s, size_t len, Word *out)
{
    size_t i = 0;
    while (i < len) {
        if (s[i] == QUOTE_MARK) {
            size_t j = i + 1;
            while (j < len && s[j] != QUOTE_MARK)
                j++;
            size_t clen = j - i - 1;
            char *content = xmalloc(clen + 1);
            memcpy(content, s + i + 1, clen);
            content[clen] = '\0';
            if (content[0] == '$' && content[1] == '{') {
                word_add_piece(out, PIECE_PARAM, 1, content + 1);
            } else if (content[0] == '$' && content[1] == '(' &&
                       content[2] == '(') {
                size_t blen = clen >= 5 ? clen - 5 : 0;
                char *body = xmalloc(blen + 1);
                memcpy(body, content + 3, blen);
                body[blen] = '\0';
                word_add_piece(out, PIECE_ARITH, 1, body);
                free(body);
            } else if (content[0] == '$' && content[1] == '(') {
                size_t blen = clen >= 3 ? clen - 3 : 0;
                char *body = xmalloc(blen + 1);
                memcpy(body, content + 2, blen);
                body[blen] = '\0';
                word_add_piece(out, PIECE_CMDSUB, 1, body);
                free(body);
            } else if (content[0] == '$') {
                word_add_piece(out, PIECE_PARAM, 1, content + 1);
            } else {
                word_add_piece(out, PIECE_TEXT, 1, content);
            }
            free(content);
            i = j + 1;
            continue;
        }
        size_t j = i;
        while (j < len && s[j] != QUOTE_MARK)
            j++;
        char *run = xmalloc(j - i + 1);
        memcpy(run, s + i, j - i);
        run[j - i] = '\0';
        if (run[0]) {
            Word w;
            if (lex_single_word(run, &w) == 0) {
                for (int k = 0; k < w.count; k++)
                    word_add_piece(out, w.items[k].kind, w.items[k].quoted,
                                   w.items[k].text);
                word_free(&w);
            } else {
                word_append_text(out, 0, run, strlen(run));
            }
        }
        free(run);
        i = j;
    }
}

static void flat_split_alts(const char *content, size_t clen, char ***alts,
                            int *count)
{
    int depth = 0;
    size_t seg_start = 0;
    size_t i = 0;
    for (;;) {
        if (i == clen || (content[i] == ',' && depth == 0)) {
            *alts = xrealloc(*alts, (*count + 1) * sizeof(char *));
            char *alt = xmalloc(i - seg_start + 1);
            memcpy(alt, content + seg_start, i - seg_start);
            alt[i - seg_start] = '\0';
            (*alts)[(*count)++] = alt;
            if (i == clen)
                break;
            seg_start = i + 1;
            i++;
            continue;
        }
        if (content[i] == '{')
            depth++;
        else if (content[i] == '}')
            depth--;
        i = flat_skip(content, i, clen);
    }
}

static void brace_expand_flat(const char *s, size_t len, Word **out,
                              int *out_count)
{
    if (*out_count >= 1024)
        return;
    size_t gs, ge;
    if (!brace_find_group_flat(s, len, &gs, &ge)) {
        Word w;
        word_init(&w);
        unflatten_to_word(s, len, &w);
        (*out)[(*out_count)++] = w;
        return;
    }
    size_t clen = ge - gs - 1;
    char *content = xmalloc(clen + 1);
    memcpy(content, s + gs + 1, clen);
    content[clen] = '\0';
    char **alts = NULL;
    int alt_count = 0;
    if (flat_is_range(content)) {
        char *dots = strstr(content, "..");
        char *a = content;
        *dots = '\0';
        char *b = dots + 2;
        char *stepstr = strstr(b, "..");
        long long step = 1;
        if (stepstr) {
            *stepstr = '\0';
            step = strtoll(stepstr + 2, NULL, 10);
            if (step == 0)
                step = 1;
        }
        long long lo = strtoll(a, NULL, 10);
        long long hi = strtoll(b, NULL, 10);
        if (step < 0)
            step = -step;
        if (lo <= hi) {
            for (long long v = lo; v <= hi && alt_count < 1024; v += step) {
                alts = xrealloc(alts, (alt_count + 1) * sizeof(char *));
                alts[alt_count++] = llstr(v);
            }
        } else {
            for (long long v = lo; v >= hi && alt_count < 1024; v -= step) {
                alts = xrealloc(alts, (alt_count + 1) * sizeof(char *));
                alts[alt_count++] = llstr(v);
            }
        }
    } else {
        flat_split_alts(content, clen, &alts, &alt_count);
    }
    for (int a = 0; a < alt_count; a++) {
        StrBuf nb;
        sb_init(&nb);
        sb_append_n(&nb, s, gs);
        sb_append(&nb, alts[a]);
        sb_append_n(&nb, s + ge + 1, len - ge - 1);
        brace_expand_flat(nb.data ? nb.data : "", nb.len, out, out_count);
        sb_free(&nb);
        free(alts[a]);
    }
    free(alts);
    free(content);
}

static void brace_expand(const Word *w, Word **out, int *out_count)
{
    StrBuf flat;
    sb_init(&flat);
    flatten_word(w, &flat);
    *out = xmalloc(1024 * sizeof(Word));
    *out_count = 0;
    brace_expand_flat(flat.data ? flat.data : "", flat.len, out, out_count);
    sb_free(&flat);
}

char **expand_word_fields(const Word *w, int flags, int *count)
{
    *count = 0;
    if (w->count == 1 && w->items[0].kind == PIECE_PARAM &&
        w->items[0].quoted && strcmp(w->items[0].text, "@") == 0) {
        *count = shell.positional_count;
        if (shell.positional_count == 0)
            return NULL;
        char **fields = xmalloc(shell.positional_count * sizeof(char *));
        for (int i = 0; i < shell.positional_count; i++)
            fields[i] = xstrdup(shell.positional[i]);
        return fields;
    }
    Word *bwords = NULL;
    int bcount = 0;
    brace_expand(w, &bwords, &bcount);
    FieldList out;
    memset(&out, 0, sizeof(out));
    int failed = 0;
    for (int i = 0; i < bcount && !failed; i++) {
        if (expand_one_fields(&bwords[i], flags, &out) < 0)
            failed = 1;
        word_free(&bwords[i]);
    }
    free(bwords);
    if (failed) {
        for (int i = 0; i < out.count; i++)
            free(out.items[i]);
        free(out.items);
        *count = -1;
        return NULL;
    }
    *count = out.count;
    return out.items;
}

void free_fields(char **fields, int count)
{
    for (int i = 0; i < count; i++)
        free(fields[i]);
    free(fields);
}

char *expand_heredoc(const char *body)
{
    Word w;
    lex_heredoc_body(body, &w);
    char *out = expand_word_string(&w, EXP_NOSPLIT | EXP_NOGLOB);
    word_free(&w);
    return out ? out : xstrdup(body);
}

typedef struct {
    char cwd[PATH_MAX];
    char host[256];
    const char *user;
} PromptContext;

static int prompt_color_code(const char *name, size_t len)
{
    static const struct {
        const char *name;
        int code;
    } table[] = {
        {"black", 30},           {"red", 31},
        {"green", 32},           {"yellow", 33},
        {"blue", 34},            {"magenta", 35},
        {"cyan", 36},            {"white", 37},
        {"bright-black", 90},    {"bright-red", 91},
        {"bright-green", 92},    {"bright-yellow", 93},
        {"bright-blue", 94},     {"bright-magenta", 95},
        {"bright-cyan", 96},     {"bright-white", 97},
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++)
        if (strlen(table[i].name) == len &&
            strncmp(table[i].name, name, len) == 0)
            return table[i].code;
    return -1;
}

static void prompt_append_color(StrBuf *sb, int code)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "\x1b[%dm", code);
    sb_append(sb, buf);
}

static char *read_first_line(const char *path, char *buf, size_t size)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return NULL;
    if (!fgets(buf, (int)size, f)) {
        fclose(f);
        return NULL;
    }
    fclose(f);
    size_t n = strlen(buf);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        buf[--n] = '\0';
    return buf;
}

static char *git_head_path(const char *cwd, char *out, size_t size)
{
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", cwd);
    for (;;) {
        char git[PATH_MAX + 8];
        snprintf(git, sizeof(git), "%s/.git", dir);
        struct stat st;
        if (stat(git, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                snprintf(out, size, "%s/HEAD", git);
                return out;
            }
            char line[PATH_MAX];
            if (!read_first_line(git, line, sizeof(line)))
                return NULL;
            if (strncmp(line, "gitdir: ", 8) != 0)
                return NULL;
            snprintf(out, size, "%s/HEAD", line + 8);
            return out;
        }
        char *slash = strrchr(dir, '/');
        if (!slash || slash == dir)
            return NULL;
        *slash = '\0';
    }
}

static char *git_branch(const char *cwd)
{
    char path[PATH_MAX];
    char line[512];
    if (!git_head_path(cwd, path, sizeof(path)))
        return NULL;
    if (!read_first_line(path, line, sizeof(line)))
        return NULL;
    if (strncmp(line, "ref: refs/heads/", 16) == 0)
        return xstrdup(line + 16);
    if (strlen(line) >= 7) {
        char *short_sha = xmalloc(8);
        memcpy(short_sha, line, 7);
        short_sha[7] = '\0';
        return short_sha;
    }
    return NULL;
}

static void prompt_context_init(PromptContext *ctx)
{
    if (!getcwd(ctx->cwd, sizeof(ctx->cwd)))
        strcpy(ctx->cwd, "?");
    if (gethostname(ctx->host, sizeof(ctx->host)) != 0)
        strcpy(ctx->host, "localhost");
    ctx->user = var_get("USER");
    struct passwd *pw = getpwuid(getuid());
    if (!ctx->user && pw)
        ctx->user = pw->pw_name;
    if (!ctx->user)
        ctx->user = "user";
}

static void prompt_append_text(StrBuf *sb, const char *fmt,
                               const PromptContext *ctx, int depth);

static void prompt_append_conditional(StrBuf *sb, const char *start,
                                      const PromptContext *ctx, int depth,
                                      const char **next)
{
    const char *p = start;
    int condition = 0;
    if (*p == '?')
        condition = shell.last_status == 0;
    const char *sep1 = strchr(p, '.');
    if (!sep1) {
        *next = p + 1;
        return;
    }
    const char *true_start = sep1 + 1;
    const char *sep2 = strchr(true_start, '.');
    const char *close = sep2 ? strchr(sep2, ')') : strchr(true_start, ')');
    if (!close) {
        *next = p + 1;
        return;
    }
    const char *chosen;
    const char *chosen_end;
    if (condition) {
        chosen = true_start;
        chosen_end = sep2 ? sep2 : close;
    } else {
        chosen = sep2 ? sep2 + 1 : close;
        chosen_end = close;
    }
    if (chosen < chosen_end) {
        char *text = xmalloc(chosen_end - chosen + 1);
        memcpy(text, chosen, chosen_end - chosen);
        text[chosen_end - chosen] = '\0';
        prompt_append_text(sb, text, ctx, depth + 1);
        free(text);
    }
    *next = close + 1;
}

static void prompt_append_text(StrBuf *sb, const char *fmt,
                               const PromptContext *ctx, int depth)
{
    if (depth > 8)
        return;
    for (const char *p = fmt; *p; p++) {
        if (*p != '%' || !p[1]) {
            sb_append_char(sb, *p);
            continue;
        }
        p++;
        switch (*p) {
        case 'n':
            sb_append(sb, ctx->user);
            break;
        case 'm': {
            char *dot = strchr(ctx->host, '.');
            if (dot)
                sb_append_n(sb, ctx->host, dot - ctx->host);
            else
                sb_append(sb, ctx->host);
            break;
        }
        case 'M':
            sb_append(sb, ctx->host);
            break;
        case '~': {
            const char *home = var_get("HOME");
            size_t hlen = home ? strlen(home) : 0;
            if (hlen > 0 && strncmp(ctx->cwd, home, hlen) == 0 &&
                (ctx->cwd[hlen] == '/' || ctx->cwd[hlen] == '\0')) {
                sb_append_char(sb, '~');
                sb_append(sb, ctx->cwd + hlen);
            } else {
                sb_append(sb, ctx->cwd);
            }
            break;
        }
        case '/':
            sb_append(sb, ctx->cwd);
            break;
        case '#':
            sb_append_char(sb, geteuid() == 0 ? '#' : '%');
            break;
        case '?': {
            char *s = llstr(shell.last_status);
            sb_append(sb, s);
            free(s);
            break;
        }
        case 'F': {
            const char *open = strchr(p, '{');
            const char *close = open ? strchr(open, '}') : NULL;
            if (open && close) {
                int code = prompt_color_code(open + 1, close - open - 1);
                if (code >= 0)
                    prompt_append_color(sb, code);
                p = close;
            }
            break;
        }
        case 'f':
            prompt_append_color(sb, 0);
            break;
        case 'K': {
            const char *open = strchr(p, '{');
            const char *close = open ? strchr(open, '}') : NULL;
            if (open && close) {
                size_t len = close - open - 1;
                if (len == 7 && strncmp(open + 1, "default", 7) == 0) {
                    prompt_append_color(sb, 49);
                } else {
                    int code = prompt_color_code(open + 1, len);
                    if (code >= 0)
                        prompt_append_color(sb, code + 10);
                }
                p = close;
            }
            break;
        }
        case 'k':
            prompt_append_color(sb, 49);
            break;
        case 'G': {
            char *branch = git_branch(ctx->cwd);
            if (branch) {
                sb_append(sb, branch);
                free(branch);
            }
            break;
        }
        case 'g': {
            char *branch = git_branch(ctx->cwd);
            if (branch) {
                sb_append(sb, " (");
                sb_append(sb, branch);
                sb_append_char(sb, ')');
                free(branch);
            }
            break;
        }
        case 'P':
            sb_append(sb, "\xee\x82\xb0");
            break;
        case 'C':
            sb_append(sb, "\xe2\x9d\xaf");
            break;
        case '(':
            prompt_append_conditional(sb, p + 1, ctx, depth, &p);
            p--;
            break;
        case '{': {
            const char *end = strstr(p + 1, "%}");
            if (end) {
                sb_append_n(sb, p + 1, end - p - 1);
                p = end + 1;
            }
            break;
        }
        case '%':
            sb_append_char(sb, '%');
            break;
        default:
            sb_append_char(sb, '%');
            sb_append_char(sb, *p);
            break;
        }
    }
}

char *expand_prompt(const char *fmt)
{
    PromptContext ctx;
    prompt_context_init(&ctx);
    StrBuf sb;
    sb_init(&sb);
    prompt_append_text(&sb, fmt, &ctx, 0);
    return sb_take(&sb);
}
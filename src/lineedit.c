#include "ash.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#define MAX_HISTORY 2000

enum {
    KEY_UP = 1000,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_HOME,
    KEY_END,
    KEY_DELETE,
    KEY_CTRL_LEFT,
    KEY_CTRL_RIGHT,
    KEY_ALT_B,
    KEY_ALT_F,
    KEY_ALT_D,
    KEY_ALT_BACKSPACE,
    KEY_ESC
};

static char *history[MAX_HISTORY];
static int hist_total;
static char *hist_path;

static struct termios orig_termios;
static int cols = 80;

static char *buf;
static int len;
static int cap;
static int cursor;

static char *killbuf;
static char *current_prompt;
static int last_cursor_row;

static int hist_index;
static char *hist_draft;

void lineedit_init(void)
{
    const char *home = var_get("HOME");
    StrBuf sb;
    sb_init(&sb);
    sb_append(&sb, home ? home : ".");
    sb_append(&sb, "/.ash_history");
    hist_path = sb_take(&sb);
    FILE *f = fopen(hist_path, "r");
    if (!f)
        return;
    fchmod(fileno(f), 0600);
    char *line = NULL;
    size_t linecap = 0;
    ssize_t n;
    while ((n = getline(&line, &linecap, f)) > 0) {
        if (n > 0 && line[n - 1] == '\n')
            line[n - 1] = '\0';
        if (!line[0])
            continue;
        if (hist_total == MAX_HISTORY) {
            free(history[0]);
            memmove(history, history + 1, (MAX_HISTORY - 1) * sizeof(char *));
            hist_total--;
        }
        history[hist_total++] = xstrdup(line);
    }
    free(line);
    fclose(f);
}

int hist_count(void)
{
    return hist_total;
}

const char *hist_line(int index)
{
    return history[index];
}

void hist_clear(void)
{
    for (int i = 0; i < hist_total; i++)
        free(history[i]);
    hist_total = 0;
}

static void hist_add(const char *line)
{
    if (hist_total == MAX_HISTORY) {
        free(history[0]);
        memmove(history, history + 1, (MAX_HISTORY - 1) * sizeof(char *));
        hist_total--;
    }
    history[hist_total++] = xstrdup(line);
    int fd = open(hist_path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0)
        return;
    fchmod(fd, 0600);
    FILE *f = fdopen(fd, "a");
    if (f) {
        fprintf(f, "%s\n", line);
        fclose(f);
    } else {
        close(fd);
    }
}

static void raw_on(void)
{
    tcgetattr(0, &orig_termios);
    struct termios raw = orig_termios;
    raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_oflag &= ~(OPOST);
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &raw);
}

static void raw_off(void)
{
    tcsetattr(0, TCSANOW, &orig_termios);
}

static int read_key(void)
{
    unsigned char c;
    ssize_t n = read(0, &c, 1);
    if (n <= 0)
        return -1;
    if (c != 27)
        return c;
    struct termios t;
    tcgetattr(0, &t);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 1;
    tcsetattr(0, TCSANOW, &t);
    unsigned char seq[6];
    int result = KEY_ESC;
    int n1 = read(0, seq, 1);
    if (n1 == 1) {
        if (seq[0] == 'O') {
            int n2 = read(0, seq + 1, 1);
            if (n2 == 1) {
                if (seq[1] == 'A')
                    result = KEY_UP;
                else if (seq[1] == 'B')
                    result = KEY_DOWN;
                else if (seq[1] == 'C')
                    result = KEY_RIGHT;
                else if (seq[1] == 'D')
                    result = KEY_LEFT;
                else if (seq[1] == 'H')
                    result = KEY_HOME;
                else if (seq[1] == 'F')
                    result = KEY_END;
            }
        } else if (seq[0] == '[') {
            int n2 = read(0, seq + 1, 1);
            if (n2 == 1) {
                char f = seq[1];
                if (f == 'A')
                    result = KEY_UP;
                else if (f == 'B')
                    result = KEY_DOWN;
                else if (f == 'C')
                    result = KEY_RIGHT;
                else if (f == 'D')
                    result = KEY_LEFT;
                else if (f == 'H')
                    result = KEY_HOME;
                else if (f == 'F')
                    result = KEY_END;
                else if (f >= '0' && f <= '9') {
                    int n3 = read(0, seq + 2, 1);
                    if (n3 == 1) {
                        if (seq[2] == '~') {
                            if (f == '1' || f == '7')
                                result = KEY_HOME;
                            else if (f == '3')
                                result = KEY_DELETE;
                            else if (f == '4' || f == '8')
                                result = KEY_END;
                        } else if (seq[2] == ';') {
                            int n4 = read(0, seq + 3, 2);
                            if (n4 == 2) {
                                char fin = seq[4];
                                int mod = seq[3] - '0';
                                if (fin == 'C')
                                    result = mod == 5 ? KEY_CTRL_RIGHT
                                                      : KEY_RIGHT;
                                else if (fin == 'D')
                                    result = mod == 5 ? KEY_CTRL_LEFT
                                                      : KEY_LEFT;
                                else if (fin == 'A')
                                    result = KEY_UP;
                                else if (fin == 'B')
                                    result = KEY_DOWN;
                                else if (fin == 'H')
                                    result = KEY_HOME;
                                else if (fin == 'F')
                                    result = KEY_END;
                            }
                        }
                    }
                }
            }
        } else if (seq[0] == 'b') {
            result = KEY_ALT_B;
        } else if (seq[0] == 'f') {
            result = KEY_ALT_F;
        } else if (seq[0] == 'd') {
            result = KEY_ALT_D;
        } else if (seq[0] == 127) {
            result = KEY_ALT_BACKSPACE;
        }
    }
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &t);
    return result;
}

static void buf_reset(void)
{
    len = 0;
    cursor = 0;
    if (buf)
        buf[0] = '\0';
}

static void buf_insert(const char *text, int n)
{
    if (len + n + 1 > cap) {
        cap = (len + n + 1) * 2;
        buf = xrealloc(buf, cap);
    }
    if (len > cursor)
        memmove(buf + cursor + n, buf + cursor, len - cursor);
    memcpy(buf + cursor, text, n);
    len += n;
    cursor += n;
    buf[len] = '\0';
}

static void buf_delete(int start, int end)
{
    if (start < 0)
        start = 0;
    if (end > len)
        end = len;
    if (start >= end)
        return;
    memmove(buf + start, buf + end, len - end);
    len -= end - start;
    buf[len] = '\0';
    if (cursor > end)
        cursor -= end - start;
    else if (cursor > start)
        cursor = start;
}

static int prev_char_start(int pos)
{
    pos--;
    while (pos > 0 && (buf[pos] & 0xC0) == 0x80)
        pos--;
    return pos < 0 ? 0 : pos;
}

static int next_char_end(int pos)
{
    pos++;
    while (pos < len && (buf[pos] & 0xC0) == 0x80)
        pos++;
    return pos;
}

static int word_left(int pos)
{
    while (pos > 0 && isspace((unsigned char)buf[pos - 1]))
        pos--;
    while (pos > 0 && !isspace((unsigned char)buf[pos - 1]))
        pos--;
    return pos;
}

static int word_right(int pos)
{
    while (pos < len && isspace((unsigned char)buf[pos]))
        pos++;
    while (pos < len && !isspace((unsigned char)buf[pos]))
        pos++;
    return pos;
}

static void set_kill(const char *text, int n)
{
    free(killbuf);
    killbuf = xmalloc(n + 1);
    memcpy(killbuf, text, n);
    killbuf[n] = '\0';
}

static int visible_width(const char *s, int n)
{
    int width = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] == 27) {
            i++;
            if (i < n && s[i] == '[') {
                i++;
                while (i < n && !isalpha((unsigned char)s[i]))
                    i++;
            }
            continue;
        }
        if (s[i] == '%' && i + 1 < n && s[i + 1] == '{') {
            i += 2;
            while (i + 1 < n && !(s[i] == '%' && s[i + 1] == '}'))
                i++;
            i++;
            continue;
        }
        if ((s[i] & 0xC0) != 0x80)
            width++;
    }
    return width;
}

static int command_in_path(const char *name)
{
    if (strchr(name, '/'))
        return access(name, X_OK) == 0;
    const char *path = var_get("PATH");
    if (!path)
        path = "/usr/local/bin:/usr/bin:/bin";
    char *copy = xstrdup(path);
    char *save = NULL;
    int found = 0;
    for (char *dir = strtok_r(copy, ":", &save); dir;
         dir = strtok_r(NULL, ":", &save)) {
        char full[4096];
        snprintf(full, sizeof(full), "%s/%s", dir[0] ? dir : ".", name);
        if (access(full, X_OK) == 0) {
            found = 1;
            break;
        }
    }
    free(copy);
    return found;
}

static int is_reserved_word(const char *s)
{
    static const char *words[] = {
        "if", "then", "elif", "else", "fi", "for", "while", "until",
        "do", "done", "case", "esac", "in", "function", "{", "}", "!"
    };
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++)
        if (strcmp(s, words[i]) == 0)
            return 1;
    return 0;
}

static void append_color(StrBuf *out, int color)
{
    char tmp[16];
    snprintf(tmp, sizeof(tmp), "\x1b[%dm", color);
    sb_append(out, tmp);
}

static void append_reset(StrBuf *out)
{
    sb_append(out, "\x1b[0m");
}

static int scan_expansion_end(int start)
{
    int i = start + 1;
    if (i >= len)
        return i;
    if (buf[i] == '{') {
        int depth = 1;
        i++;
        while (i < len && depth > 0) {
            if (buf[i] == '{')
                depth++;
            else if (buf[i] == '}')
                depth--;
            i++;
        }
        return i;
    }
    if (buf[i] == '(') {
        int depth = 1;
        i++;
        while (i < len && depth > 0) {
            if (buf[i] == '(')
                depth++;
            else if (buf[i] == ')')
                depth--;
            i++;
        }
        return i;
    }
    if (isalpha((unsigned char)buf[i]) || buf[i] == '_') {
        while (i < len && (isalnum((unsigned char)buf[i]) || buf[i] == '_'))
            i++;
        return i;
    }
    return i + 1;
}

static void append_highlighted(StrBuf *out, int *width)
{
    int cap = len > 0 ? len : 1;
    int *colors = xmalloc(cap * sizeof(int));
    memset(colors, 0, cap * sizeof(int));
    int i = 0;
    int expect_cmd = 1;
    while (i < len) {
        char c = buf[i];
        if (c == ' ' || c == '\t') {
            i++;
            continue;
        }
        if (c == '#' &&
            (i == 0 || strchr(" \t;|&(", buf[i - 1]))) {
            for (int k = i; k < len; k++)
                colors[k] = 90;
            break;
        }
        if (strchr(";|&<>()", c)) {
            colors[i] = 35;
            if (c == ';' || c == '|' || c == '&' || c == '(')
                expect_cmd = 1;
            i++;
            continue;
        }
        int j = i;
        while (j < len && !strchr(" \t;|&<>()", buf[j]))
            j++;
        char *word = xmalloc(j - i + 1);
        memcpy(word, buf + i, j - i);
        word[j - i] = '\0';
        int color = 0;
        int is_assign = 0;
        char *eq = strchr(word, '=');
        if (eq && eq != word) {
            is_assign = 1;
            for (char *p = word; p < eq; p++)
                if (!isalnum((unsigned char)*p) && *p != '_')
                    is_assign = 0;
        }
        if (expect_cmd && !is_assign) {
            char *name = xstrdup(word);
            for (char *p = name; *p; p++) {
                if (*p == '$' || *p == '\'' || *p == '"' || *p == '\\') {
                    *p = '\0';
                    break;
                }
            }
            if (is_reserved_word(name)) {
                color = 35;
                expect_cmd = 1;
            } else if (name[0] &&
                       (builtin_lookup(name) || function_lookup(name) ||
                        alias_get(name) || command_in_path(name))) {
                color = 32;
                expect_cmd = 0;
            } else if (name[0]) {
                color = 31;
                expect_cmd = 0;
            }
            free(name);
        } else if (is_assign) {
            expect_cmd = 1;
        } else {
            expect_cmd = 0;
        }
        int k = i;
        while (k < j) {
            char wc = buf[k];
            if (wc == '\'') {
                int end = k + 1;
                while (end < j && buf[end] != '\'')
                    end++;
                if (end < j)
                    end++;
                for (int p = k; p < end; p++)
                    colors[p] = 33;
                k = end;
                continue;
            }
            if (wc == '"') {
                colors[k] = 33;
                k++;
                while (k < j && buf[k] != '"') {
                    if (buf[k] == '$') {
                        int end = scan_expansion_end(k);
                        if (end > j)
                            end = j;
                        for (int p = k; p < end; p++)
                            colors[p] = 36;
                        k = end;
                        continue;
                    }
                    colors[k] = 33;
                    k++;
                }
                if (k < j) {
                    colors[k] = 33;
                    k++;
                }
                continue;
            }
            if (wc == '$') {
                int end = scan_expansion_end(k);
                if (end > j)
                    end = j;
                for (int p = k; p < end; p++)
                    colors[p] = 36;
                k = end;
                continue;
            }
            if (strchr("*?[", wc)) {
                colors[k] = 34;
                k++;
                continue;
            }
            colors[k] = color;
            k++;
        }
        free(word);
        i = j;
    }
    i = 0;
    while (i < len) {
        int c = colors[i];
        int j = i + 1;
        while (j < len && colors[j] == c)
            j++;
        if (c)
            append_color(out, c);
        sb_append_n(out, buf + i, j - i);
        if (c)
            append_reset(out);
        i = j;
    }
    *width += visible_width(buf, len);
    free(colors);
}

static void update_cols(void)
{
    struct winsize ws;
    if (ioctl(0, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        cols = ws.ws_col;
}

static void redisplay(void)
{
    update_cols();
    StrBuf out;
    sb_init(&out);
    if (last_cursor_row > 0) {
        char tmp[32];
        snprintf(tmp, sizeof(tmp), "\x1b[%dA", last_cursor_row);
        sb_append(&out, tmp);
    }
    sb_append(&out, "\r\x1b[J");
    sb_append(&out, current_prompt);
    int prompt_vis = visible_width(current_prompt, strlen(current_prompt));
    int buf_vis = 0;
    append_highlighted(&out, &buf_vis);
    int total = prompt_vis + buf_vis;
    const char *rps1 = var_get("RPS1");
    char *rps = NULL;
    int rps_vis = 0;
    if (rps1 && rps1[0]) {
        rps = expand_prompt(rps1);
        rps_vis = visible_width(rps, strlen(rps));
        if (rps[0] && total + 2 + rps_vis <= cols) {
            for (int i = total; i < cols - rps_vis; i++)
                sb_append_char(&out, ' ');
            sb_append(&out, rps);
        } else {
            free(rps);
            rps = NULL;
        }
    }
    int cursor_vis = prompt_vis + visible_width(buf, cursor);
    int current_row = total > 0 ? (total - 1) / cols : 0;
    int target_row = cursor_vis / cols;
    int target_col = cursor_vis % cols;
    if (current_row > target_row) {
        char tmp[32];
        snprintf(tmp, sizeof(tmp), "\x1b[%dA", current_row - target_row);
        sb_append(&out, tmp);
    }
    sb_append_char(&out, '\r');
    if (target_col > 0) {
        char tmp[32];
        snprintf(tmp, sizeof(tmp), "\x1b[%dC", target_col);
        sb_append(&out, tmp);
    }
    last_cursor_row = target_row;
    write(1, out.data, out.len);
    sb_free(&out);
    free(rps);
}

static void load_line(const char *text)
{
    buf_reset();
    buf_insert(text, strlen(text));
    cursor = len;
}

static void history_prev(void)
{
    if (hist_index == 0)
        return;
    if (hist_index == hist_total) {
        free(hist_draft);
        hist_draft = xstrdup(buf ? buf : "");
    }
    hist_index--;
    load_line(history[hist_index]);
}

static void history_next(void)
{
    if (hist_index >= hist_total)
        return;
    hist_index++;
    if (hist_index == hist_total)
        load_line(hist_draft ? hist_draft : "");
    else
        load_line(history[hist_index]);
}

static char *search_query;
static int search_index;
static char *search_match;

static void search_find(void)
{
    free(search_match);
    search_match = NULL;
    for (int i = search_index; i >= 0; i--) {
        if (strstr(history[i], search_query)) {
            search_match = xstrdup(history[i]);
            search_index = i;
            return;
        }
    }
    search_index = -1;
}

static void search_redisplay(void)
{
    StrBuf out;
    sb_init(&out);
    sb_append(&out, "\r\x1b[J(reverse-i-search)`");
    sb_append(&out, search_query);
    sb_append(&out, "': ");
    sb_append(&out, search_match ? search_match : "");
    write(1, out.data, out.len);
    sb_free(&out);
}

static char *run_search(int *accept)
{
    *accept = 0;
    search_query = xstrdup("");
    search_index = hist_total - 1;
    search_match = NULL;
    search_find();
    search_redisplay();
    for (;;) {
        int key = read_key();
        if (key == -1) {
            free(search_query);
            free(search_match);
            search_query = NULL;
            search_match = NULL;
            return NULL;
        }
        if (key == 13 || key == 10) {
            char *result = search_match ? xstrdup(search_match) : NULL;
            free(search_query);
            free(search_match);
            search_query = NULL;
            search_match = NULL;
            *accept = 1;
            return result;
        }
        if (key == 18) {
            search_index--;
            search_find();
            search_redisplay();
            continue;
        }
        if (key == KEY_ESC || key == KEY_UP || key == KEY_DOWN ||
            key == KEY_LEFT || key == KEY_RIGHT) {
            char *result = search_match ? xstrdup(search_match) : NULL;
            free(search_query);
            free(search_match);
            search_query = NULL;
            search_match = NULL;
            return result;
        }
        if (key == 127 || key == 8) {
            int qlen = strlen(search_query);
            if (qlen > 0)
                search_query[qlen - 1] = '\0';
            search_index = hist_total - 1;
            search_find();
            search_redisplay();
            continue;
        }
        if (key >= 32 && key < 127) {
            char tmp[2] = { (char)key, '\0' };
            StrBuf sb;
            sb_init(&sb);
            sb_append(&sb, search_query);
            sb_append(&sb, tmp);
            free(search_query);
            search_query = sb_take(&sb);
            search_find();
            search_redisplay();
            continue;
        }
        search_redisplay();
    }
}

static int str_cmp(const void *a, const void *b)
{
    return strcmp(*(const char **)a, *(const char **)b);
}

static void matches_add(char ***matches, int *count, const char *text)
{
    *matches = xrealloc(*matches, (*count + 1) * sizeof(char *));
    (*matches)[(*count)++] = xstrdup(text);
}

static void matches_sort_unique(char **matches, int *count)
{
    if (*count < 2)
        return;
    qsort(matches, *count, sizeof(char *), str_cmp);
    int w = 1;
    for (int i = 1; i < *count; i++) {
        if (strcmp(matches[i], matches[w - 1]) == 0)
            free(matches[i]);
        else
            matches[w++] = matches[i];
    }
    *count = w;
}

static void collect_command_matches(const char *prefix, char ***matches,
                                    int *count)
{
    size_t plen = strlen(prefix);
    for (int i = 0; i < builtin_count(); i++) {
        const char *name = builtin_name(i);
        if (strncmp(name, prefix, plen) == 0)
            matches_add(matches, count, name);
    }
    for (int i = 0; i < function_count(); i++) {
        const char *name = function_name(i);
        if (strncmp(name, prefix, plen) == 0)
            matches_add(matches, count, name);
    }
    for (int i = 0; i < alias_count(); i++) {
        const char *name = alias_name(i);
        if (strncmp(name, prefix, plen) == 0)
            matches_add(matches, count, name);
    }
    const char *path = var_get("PATH");
    if (!path)
        path = "/usr/local/bin:/usr/bin:/bin";
    char *copy = xstrdup(path);
    char *save = NULL;
    for (char *dir = strtok_r(copy, ":", &save); dir;
         dir = strtok_r(NULL, ":", &save)) {
        DIR *d = opendir(dir[0] ? dir : ".");
        if (!d)
            continue;
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (strncmp(ent->d_name, prefix, plen) == 0)
                matches_add(matches, count, ent->d_name);
        }
        closedir(d);
    }
    free(copy);
}

static void collect_file_matches(const char *prefix, char ***matches,
                                 int *count)
{
    const char *slash = strrchr(prefix, '/');
    char *dirpart;
    const char *base;
    if (slash) {
        dirpart = xmalloc(slash - prefix + 2);
        memcpy(dirpart, prefix, slash - prefix + 1);
        dirpart[slash - prefix + 1] = '\0';
        base = slash + 1;
    } else {
        dirpart = xstrdup("");
        base = prefix;
    }
    char *scanpath;
    const char *home = var_get("HOME");
    if (dirpart[0] == '~' && (dirpart[1] == '/' || dirpart[1] == '\0') &&
        home) {
        StrBuf sb;
        sb_init(&sb);
        sb_append(&sb, home);
        sb_append(&sb, dirpart + 1);
        scanpath = sb_take(&sb);
    } else {
        scanpath = xstrdup(dirpart[0] ? dirpart : ".");
    }
    size_t baselen = strlen(base);
    DIR *d = opendir(scanpath);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 ||
                strcmp(ent->d_name, "..") == 0)
                continue;
            if (ent->d_name[0] == '.' && base[0] != '.')
                continue;
            if (strncmp(ent->d_name, base, baselen) == 0) {
                StrBuf sb;
                sb_init(&sb);
                sb_append(&sb, dirpart);
                sb_append(&sb, ent->d_name);
                matches_add(matches, count, sb.data);
                sb_free(&sb);
            }
        }
        closedir(d);
    }
    free(scanpath);
    free(dirpart);
}

static void show_match_list(char **matches, int count)
{
    int maxw = 0;
    for (int i = 0; i < count; i++) {
        int w = strlen(matches[i]);
        if (w > maxw)
            maxw = w;
    }
    int per_row = cols / (maxw + 2);
    if (per_row < 1)
        per_row = 1;
    StrBuf out;
    sb_init(&out);
    sb_append(&out, "\r\n");
    for (int i = 0; i < count; i++) {
        sb_append(&out, matches[i]);
        if ((i + 1) % per_row == 0 || i == count - 1)
            sb_append(&out, "\r\n");
        else
            for (int p = strlen(matches[i]); p < maxw + 2; p++)
                sb_append_char(&out, ' ');
    }
    write(1, out.data, out.len);
    sb_free(&out);
    last_cursor_row = 0;
}

static void complete(void)
{
    int start = cursor;
    while (start > 0 && !isspace((unsigned char)buf[start - 1]))
        start--;
    char *prefix = xmalloc(cursor - start + 1);
    memcpy(prefix, buf + start, cursor - start);
    prefix[cursor - start] = '\0';
    int is_command = 1;
    for (int i = 0; i < start; i++) {
        if (!isspace((unsigned char)buf[i]))
            is_command = strchr(";|&(", buf[i]) != NULL;
    }
    char **matches = NULL;
    int count = 0;
    if (is_command && !strchr(prefix, '/'))
        collect_command_matches(prefix, &matches, &count);
    else
        collect_file_matches(prefix, &matches, &count);
    matches_sort_unique(matches, &count);
    if (count == 0) {
        free(prefix);
        free(matches);
        return;
    }
    if (count == 1) {
        char *match = xstrdup(matches[0]);
        buf_delete(start, cursor);
        cursor = start;
        buf_insert(match, strlen(match));
        char *expanded = match;
        StrBuf sb;
        sb_init(&sb);
        const char *home = var_get("HOME");
        if (match[0] == '~' && home) {
            sb_append(&sb, home);
            sb_append(&sb, match + 1);
            expanded = sb.data;
        }
        struct stat st;
        int is_dir = stat(expanded, &st) == 0 && S_ISDIR(st.st_mode);
        if (is_dir)
            buf_insert("/", 1);
        else
            buf_insert(" ", 1);
        sb_free(&sb);
        free(match);
    } else {
        int lcp = strlen(prefix);
        for (;;) {
            char c = matches[0][lcp];
            if (!c)
                break;
            int all = 1;
            for (int i = 1; i < count; i++)
                if (matches[i][lcp] != c)
                    all = 0;
            if (!all)
                break;
            lcp++;
        }
        if (lcp > (int)strlen(prefix)) {
            buf_delete(start, cursor);
            cursor = start;
            buf_insert(matches[0], lcp);
        } else {
            show_match_list(matches, count);
        }
    }
    for (int i = 0; i < count; i++)
        free(matches[i]);
    free(matches);
    free(prefix);
}

char *lineedit_read(const char *prompt)
{
    current_prompt = (char *)prompt;
    buf_reset();
    hist_index = hist_total;
    free(hist_draft);
    hist_draft = NULL;
    last_cursor_row = 0;
    raw_on();
    redisplay();
    for (;;) {
        int key = read_key();
        if (key == -1) {
            raw_off();
            return NULL;
        }
        if (key == 13 || key == 10)
            break;
        if (key == 4) {
            if (len == 0) {
                raw_off();
                return NULL;
            }
            buf_delete(cursor, next_char_end(cursor));
        } else if (key == 3) {
            buf_reset();
        } else if (key == 9) {
            complete();
        } else if (key == 18) {
            int accept = 0;
            char *found = run_search(&accept);
            last_cursor_row = 0;
            if (found) {
                load_line(found);
                free(found);
                if (accept)
                    break;
            }
        } else if (key == 12) {
            write(1, "\x1b[2J\x1b[H", 7);
            last_cursor_row = 0;
        } else if (key == 1 || key == KEY_HOME) {
            cursor = 0;
        } else if (key == 5 || key == KEY_END) {
            cursor = len;
        } else if (key == 2 || key == KEY_LEFT) {
            if (cursor > 0)
                cursor = prev_char_start(cursor);
        } else if (key == 6 || key == KEY_RIGHT) {
            if (cursor < len)
                cursor = next_char_end(cursor);
        } else if (key == KEY_CTRL_LEFT || key == KEY_ALT_B) {
            cursor = word_left(cursor);
        } else if (key == KEY_CTRL_RIGHT || key == KEY_ALT_F) {
            cursor = word_right(cursor);
        } else if (key == 127 || key == 8 || key == KEY_ALT_BACKSPACE) {
            if (cursor > 0) {
                int start = prev_char_start(cursor);
                if (key == KEY_ALT_BACKSPACE)
                    start = word_left(cursor);
                buf_delete(start, cursor);
            }
        } else if (key == KEY_DELETE) {
            buf_delete(cursor, next_char_end(cursor));
        } else if (key == 11) {
            set_kill(buf + cursor, len - cursor);
            buf_delete(cursor, len);
        } else if (key == 21) {
            set_kill(buf, cursor);
            buf_delete(0, cursor);
        } else if (key == 23) {
            int start = word_left(cursor);
            set_kill(buf + start, cursor - start);
            buf_delete(start, cursor);
        } else if (key == KEY_ALT_D) {
            int end = word_right(cursor);
            set_kill(buf + cursor, end - cursor);
            buf_delete(cursor, end);
        } else if (key == 25) {
            if (killbuf)
                buf_insert(killbuf, strlen(killbuf));
        } else if (key == 16 || key == KEY_UP) {
            history_prev();
        } else if (key == 14 || key == KEY_DOWN) {
            history_next();
        } else if (key == 20) {
            if (cursor > 0 && cursor < len) {
                char tmp = buf[cursor - 1];
                buf[cursor - 1] = buf[cursor];
                buf[cursor] = tmp;
                cursor++;
            }
        } else if (key >= 32) {
            char tmp[2] = { (char)key, '\0' };
            buf_insert(tmp, 1);
        }
        redisplay();
    }
    raw_off();
    write(1, "\r\n", 2);
    char *result = xstrdup(buf ? buf : "");
    if (result[0])
        hist_add(result);
    return result;
}
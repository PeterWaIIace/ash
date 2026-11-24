#!/usr/bin/env bash
cd "$(dirname "$0")/.." || exit 1
ASH=./ash
pass=0
fail=0
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"; [[ -n "${agent_pid:-}" ]] && kill "$agent_pid" 2>/dev/null' EXIT

check() {
    local name=$1 want_status=$2 want_out=$3 input=$4
    local out status
    out=$(printf '%s' "$input" | "$ASH" 2>"$tmp/stderr")
    status=$?
    if [[ "$out" == "$want_out" && "$status" == "$want_status" ]]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        printf 'FAIL: %s\n' "$name"
        printf '  input:    %q\n' "$input"
        printf '  expected: status=%s out=%q\n' "$want_status" "$want_out"
        printf '  actual:   status=%s out=%q\n' "$status" "$out"
        printf '  stderr:   %s\n' "$(cat "$tmp/stderr")"
    fi
}

check_match() {
    local name=$1 want_status=$2 pattern=$3 input=$4
    local out status
    out=$(printf '%s' "$input" | "$ASH" 2>"$tmp/stderr")
    status=$?
    if [[ "$out" =~ $pattern && "$status" == "$want_status" ]]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        printf 'FAIL: %s\n' "$name"
        printf '  input:    %q\n' "$input"
        printf '  expected: status=%s pattern=%s\n' "$want_status" "$pattern"
        printf '  actual:   status=%s out=%q\n' "$status" "$out"
        printf '  stderr:   %s\n' "$(cat "$tmp/stderr")"
    fi
}

check_args() {
    local name=$1 want_status=$2 want_out=$3
    shift 3
    local out status
    out=$("$ASH" "$@" 2>"$tmp/stderr" </dev/null)
    status=$?
    if [[ "$out" == "$want_out" && "$status" == "$want_status" ]]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        printf 'FAIL: %s\n' "$name"
        printf '  expected: status=%s out=%q\n' "$want_status" "$want_out"
        printf '  actual:   status=%s out=%q\n' "$status" "$out"
        printf '  stderr:   %s\n' "$(cat "$tmp/stderr")"
    fi
}

check echo_simple 0 "hello" 'echo hello'
check exit_code 3 "" 'exit 3'
check external_status 1 "" 'false'
check unknown_command 127 "" 'no-such-command-xyz'
check double_quote 0 "a b" 'echo "a b"'
check single_quote 0 "a  b" "echo 'a  b'"
check backslash_escape 0 "a b" 'echo a\ b'
check adjacent_quotes 0 "abc" "echo \"a\"'b'c"
check semicolon 0 $'a\nb' 'echo a; echo b'
check and_if 0 "yes" 'true && echo yes'
check and_skip 1 "" 'false && echo no'
check or_if 0 "yes" 'false || echo yes'
check and_or_chain 0 "yes" 'false && echo no || echo yes'
check pipe 0 "HELLO" 'echo hello | tr a-z A-Z'
check pipe_three 0 "a" "printf 'c\nb\na\n' | sort | head -n 1"
check negate_false 0 "" '! false'
check negate_true 1 "" '! true'
check redirect_out 0 "hi" "echo hi > $tmp/f1; cat $tmp/f1"
check redirect_append 0 $'a\nb' "echo a > $tmp/f2; echo b >> $tmp/f2; cat $tmp/f2"
check redirect_in 0 "content" "echo content > $tmp/f3; cat < $tmp/f3"
check redirect_stderr 0 "err" "echo err 2> $tmp/f4; cat $tmp/f4"
check redirect_merge 0 $'out\nerr' "{ echo out; echo err >&2; } > $tmp/f5 2>&1; cat $tmp/f5"
check redirect_fd2 0 "err" "echo err 2> $tmp/f6 1>&2; cat $tmp/f6"
check heredoc 0 "hello" $'cat <<EOF\nhello\nEOF'
check heredoc_quoted 0 '$HOME' $'cat <<\'EOF\'\n$HOME\nEOF'
check heredoc_expand 0 "val=5" $'X=5\ncat <<EOF\nval=$X\nEOF'
check heredoc_strip_tabs 0 "line" $'cat <<-EOF\n\tline\n\tEOF'
check herestring 0 "hello" 'cat <<< hello'
check subshell 0 $'a\nb' '(echo a; echo b)'
check subshell_isolation 0 "1" 'x=1; (x=2); echo $x'
check group 0 $'a\nb' '{ echo a; echo b; }'
check comment 0 "a" 'echo a # comment'
check empty_input 0 "" ''

check var_basic 0 "5" 'X=5; echo $X'
check var_braces 0 "foobar" 'X=foo; echo ${X}bar'
check var_default 0 "fallback" 'echo ${UNSET_VAR_XYZ:-fallback}'
check var_default_set 0 "v" 'X=v; echo ${X:-fallback}'
check var_assign_default 0 $'assigned\nassigned' 'echo ${NEWVAR_XYZ:=assigned}; echo $NEWVAR_XYZ'
check var_alt 0 "alt" 'X=v; echo ${X:+alt}'
check var_length 0 "3" 'X=abc; echo ${#X}'
check var_strip_suffix_short 0 "foo.tar" 'X=foo.tar.gz; echo ${X%.gz}'
check var_strip_suffix_long 0 "foo" 'X=foo.tar.gz; echo ${X%%.*}'
check var_strip_prefix_short 0 "tar.gz" 'X=foo.tar.gz; echo ${X#foo.}'
check var_strip_prefix_long 0 "gz" 'X=foo.tar.gz; echo ${X##*.}'
check special_status 0 "1" 'false; echo $?'
check special_pid 0 "1" 'echo $$ | grep -E "^[0-9]+$" > /dev/null && echo 1'
check cmdsub 0 "nested" 'echo $(echo nested)'
check cmdsub_strip_newlines 0 "[x]" "echo \"[\$(printf 'x\n\n')]\""
check backticks 0 "bt" 'echo `echo bt`'
check arith 0 "14" 'echo $((2+3*4))'
check arith_parens 0 "20" 'echo $(((2+3)*4))'
check arith_var 0 "14" 'X=7; echo $((X*2))'
check arith_compare 0 "1" 'echo $((3>2))'
check arith_hex 0 "255" 'echo $((0xff))'
check tilde 0 "$HOME" 'echo ~'
check export 0 "FOO=bar" 'export FOO=bar; env | grep ^FOO='
check temp_assign 0 "FOO=new" 'FOO=new env | grep ^FOO='
check temp_assign_restore 0 "old" 'FOO=old; FOO=new true; echo $FOO'
check ifs_split 0 "3" "X='a:b:c'; IFS=:; set -- \$X; echo \$#"
check empty_unquoted_no_field 0 "0" 'UNSET_XYZ=; set -- $UNSET_XYZ; echo $#'
check quoted_empty_field 0 "1" 'set -- ""; echo $#'
check dollar_at_split 0 "" 'for a in "$@"; do echo "[$a]"; done'

check_args c_mode 0 "a b" -c 'echo $1 $2' ash a b
check_args c_mode_dollar_at 0 $'[a b]\n[c]' -c 'for a in "$@"; do echo "[$a]"; done' ash 'a b' c
check_args script_mode 0 $'script\narg1' -c 'echo script; echo $1' ash arg1

printf 'echo from-script\necho $1\n' > "$tmp/script.sh"
check_args script_file 0 $'from-script\nhello' "$tmp/script.sh" hello

check if_then 0 "yes" 'if true; then echo yes; else echo no; fi'
check if_else 0 "no" 'if false; then echo yes; else echo no; fi'
check if_elif 0 "b" 'if false; then echo a; elif true; then echo b; else echo c; fi'
check while_loop 0 $'0\n1\n2' 'i=0; while [ $i -lt 3 ]; do echo $i; i=$((i+1)); done'
check until_loop 0 $'0\n1' 'i=0; until [ $i -ge 2 ]; do echo $i; i=$((i+1)); done'
check for_loop 0 $'a\nb\nc' 'for i in a b c; do echo $i; done'
check for_no_in 0 $'x\ny' 'set -- x y; for i; do echo $i; done'
check break 0 "1" 'for i in 1 2 3; do if [ $i = 2 ]; then break; fi; echo $i; done'
check continue 0 $'1\n3' 'for i in 1 2 3; do if [ $i = 2 ]; then continue; fi; echo $i; done'
check break_two 0 "1" 'for i in 1 2; do for j in 1 2; do if [ $j = 2 ]; then break 2; fi; echo $i; done; done'
check case_exact 0 "yes" 'case foo in bar) echo no;; foo|baz) echo yes;; esac'
check case_glob 0 "match" 'case abc in a*) echo match;; esac'
check case_default 0 "def" 'case x in a) echo no;; *) echo def;; esac'
check case_empty_body 0 "" 'case x in x) ;; esac'
check function 0 "hi world" 'greet() { echo "hi $1"; }; greet world'
check function_return 0 "5" 'f() { return 5; }; f; echo $?'
check function_local 0 $'inner\nouter' 'x=outer; f() { local x=inner; echo $x; }; f; echo $x'
check function_keyword 0 "ok" 'function f { echo ok; }; f'
check test_file 0 "yes" '[ -f Makefile ] && echo yes'
check test_string 0 "eq" '[ abc = abc ] && echo eq'
check test_not 0 "yes" '[ ! -f /nonexistent-xyz ] && echo yes'
check test_and_or 0 "yes" '[ 1 -eq 1 -a 2 -eq 2 ] && echo yes'
check_args shift_builtin 0 "b" -c 'shift; echo $1' ash a b
check eval 0 "evaluated" "eval 'echo evaluated'"
check printf_basic 0 "hi-42" "printf '%s-%d\n' hi 42"
check printf_reuse 0 $'a\nb\nc' "printf '%s\n' a b c"
check printf_no_args 0 "x" "printf 'x' a b"
check read_vars 0 "one two" "read a b <<< 'one two'; echo \$a \$b"
check alias 0 "aliased" "alias ll='echo aliased'; ll"
check alias_args 0 "hello" "alias e='echo'; e hello"
check unalias 127 "" "alias x='echo a'; unalias x; x"
check type_builtin 0 "cd is a shell builtin" 'type cd'
check trap_exit 0 $'hi\nbye' "trap 'echo bye' EXIT; echo hi"
check exec_builtin 0 "replaced" 'exec echo replaced'
check source 0 "sourced" "echo 'echo sourced' > $tmp/src.sh; source $tmp/src.sh"
check errexit 1 "" 'set -e; false; echo no'
check errexit_condition 0 "ok" 'set -e; if false; then echo a; fi; echo ok'
check errexit_and_or 0 "ok" 'set -e; false || true; echo ok'
check nounset 1 "" 'set -u; echo $UNSET_XYZ'

check_match bg_wait 0 $'^\\[1\\] [0-9]+\ndone$' 'sleep 0.05 & wait; echo done'
check_match bg_last_pid 0 $'^\\[1\\] [0-9]+\n0$' 'sleep 0.05 & p=$!; wait $p; echo $?'
check_match bg_kill 0 $'^\\[1\\] [0-9]+\n143$' 'sleep 5 & kill %1; wait; echo $?'
check_match bg_exit_status 0 $'^\\[1\\] [0-9]+\n7$' "./ash -c 'exit 7' & wait \$!; echo \$?"

mkdir -p "$tmp/glob"
check glob_star 0 "a.txt b.txt" "cd $tmp/glob; touch a.txt b.txt c.log; echo *.txt"
check glob_question 0 "ax ay" "cd $tmp/glob; touch ax ay; echo a?"
check glob_class 0 "a1 a2" "cd $tmp/glob; touch a1 a2 b1; echo a[0-9]"
check glob_no_match 0 "zzz*" "cd $tmp/glob; echo zzz*"
check glob_quoted 0 "*.txt" "cd $tmp/glob; touch a.txt; echo '*.txt'"
check glob_recursive 0 "d/sub/y.c d/x.c" "cd $tmp/glob; mkdir -p d/sub; touch d/x.c d/sub/y.c; echo **/*.c"
check brace_comma 0 "a b c" 'echo {a,b,c}'
check brace_range 0 "1 2 3 4 5" 'echo {1..5}'
check brace_range_step 0 "1 3 5" 'echo {1..5..2}'
check brace_suffix 0 "f1.txt f2.txt" 'echo f{1,2}.txt'
check brace_nested 0 "a1 a2 b1 b2" 'echo {a,b}{1,2}'
check brace_quoted 0 "a b c" 'echo {a,"b c"}'
check brace_quoted_single 0 "x y z" "echo {x,'y z'}"
check brace_quoted_nested 0 "a1 a2 b c1 b c2" 'echo {a,"b c"}{1,2}'
check glob_doublestar_top 0 "sub sub/deep.txt top.txt" "mkdir -p $tmp/glob2; cd $tmp/glob2; mkdir -p sub; touch sub/deep.txt top.txt; echo **"
check noclobber 0 "a" "cd $tmp; set -C; echo a > nc.txt; echo b > nc.txt 2>/dev/null; cat nc.txt"
check clobber_force 0 "b" "cd $tmp; set -C; echo a > nc2.txt; echo b >| nc2.txt; cat nc2.txt"

check setopt_noclobber 0 "a" "cd $tmp; setopt noclobber; echo a > so.txt; echo b > so.txt 2>/dev/null; cat so.txt"
check ps1_default 0 "set" 'echo ${PS1:+set}'
check options_list 0 "errexit off" 'set -o | grep ^errexit'

if command -v python3 >/dev/null 2>&1 && command -v curl >/dev/null 2>&1; then
    python3 tests/mock_agent.py "$tmp/agent.port" &
    agent_pid=$!
    for _ in {1..100}; do
        [[ -s "$tmp/agent.port" ]] && break
        sleep 0.02
    done
    agent_home="$tmp/home_agent"
    mkdir -p "$agent_home"
    port=$(cat "$tmp/agent.port" 2>/dev/null)
    printf 'openai openai mock-gpt http://127.0.0.1:%s/v1 OPENAI_KEY\n' "$port" > "$agent_home/.ashagent"
    printf 'claude anthropic mock-claude http://127.0.0.1:%s/v1 ANTHROPIC_KEY\n' "$port" >> "$agent_home/.ashagent"

    out=$(HOME="$agent_home" "$ASH" -c 'ask -l' 2>"$tmp/agent.err")
    if [[ "$out" == *"mock-gpt"* && "$out" == *"mock-claude"* && "$out" != *"OPENAI_KEY"* ]]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        printf 'FAIL: agent_list\n  actual: %q\n' "$out"
    fi

    out=$(HOME="$agent_home" OPENAI_KEY=testkey "$ASH" -c 'ask -m mock-gpt hello' 2>"$tmp/agent.err")
    st=$?
    if [[ "$out" == "openai:hello" && "$st" == 0 ]]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        printf 'FAIL: agent_openai\n  status: %s\n  actual: %q\n  stderr: %s\n' "$st" "$out" "$(cat "$tmp/agent.err")"
    fi

    out=$(HOME="$agent_home" ANTHROPIC_KEY=testkey "$ASH" -c 'ask -m mock-claude think: ok' 2>"$tmp/agent.err")
    st=$?
    if [[ "$out" == "claude:think: ok" && "$st" == 0 ]]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        printf 'FAIL: agent_thinking_block\n  status: %s\n  actual: %q\n  stderr: %s\n' "$st" "$out" "$(cat "$tmp/agent.err")"
    fi

    out=$(HOME="$agent_home" ANTHROPIC_KEY=testkey "$ASH" -c 'ask -m mock-claude hi' 2>"$tmp/agent.err")
    st=$?
    if [[ "$out" == "claude:hi" && "$st" == 0 ]]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        printf 'FAIL: agent_anthropic\n  status: %s\n  actual: %q\n  stderr: %s\n' "$st" "$out" "$(cat "$tmp/agent.err")"
    fi

    out=$(HOME="$agent_home" OPENAI_KEY=testkey "$ASH" -c 'ask openai/mock-gpt shorthand' 2>"$tmp/agent.err")
    st=$?
    if [[ "$out" == "openai:shorthand" && "$st" == 0 ]]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        printf 'FAIL: agent_shorthand\n  status: %s\n  actual: %q\n  stderr: %s\n' "$st" "$out" "$(cat "$tmp/agent.err")"
    fi

    out=$(HOME="$agent_home" OPENAI_KEY=testkey "$ASH" -c 'ask -m mock-gpt "café \"q\""' 2>"$tmp/agent.err")
    st=$?
    if [[ "$out" == 'openai:café "q"' && "$st" == 0 ]]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        printf 'FAIL: agent_escaping\n  status: %s\n  actual: %q\n  stderr: %s\n' "$st" "$out" "$(cat "$tmp/agent.err")"
    fi

    out=$(env -u OPENAI_KEY HOME="$agent_home" "$ASH" -c 'ask -m mock-gpt hi' 2>"$tmp/agent.err")
    st=$?
    if [[ "$st" != 0 ]] && grep -q OPENAI_KEY "$tmp/agent.err"; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        printf 'FAIL: agent_missing_key\n  status: %s\n  stderr: %s\n' "$st" "$(cat "$tmp/agent.err")"
    fi

    out=$(HOME="$agent_home" OPENAI_KEY=wrong "$ASH" -c 'ask -m mock-gpt hi' 2>"$tmp/agent.err")
    st=$?
    if [[ "$st" != 0 ]] && grep -q "bad key" "$tmp/agent.err"; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        printf 'FAIL: agent_bad_key\n  status: %s\n  stderr: %s\n' "$st" "$(cat "$tmp/agent.err")"
    fi

    out=$(HOME="$agent_home" "$ASH" -c 'ask -m no-such-model hi' 2>"$tmp/agent.err")
    st=$?
    if [[ "$st" != 0 ]] && grep -q "no provider" "$tmp/agent.err"; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        printf 'FAIL: agent_unknown_model\n  status: %s\n  stderr: %s\n' "$st" "$(cat "$tmp/agent.err")"
    fi

    kill "$agent_pid" 2>/dev/null
    wait "$agent_pid" 2>/dev/null
    agent_pid=""
fi

if command -v script >/dev/null 2>&1; then
    mkdir -p "$tmp/home"
    printf "alias hi='echo from-ashrc'\n" > "$tmp/home/.ashrc"
    out=$(printf 'hi\nexit\n' | HOME="$tmp/home" script -qec "./ash" /dev/null 2>/dev/null)
    if [[ "$out" == *from-ashrc* ]]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        printf 'FAIL: ashrc_interactive\n  actual: %q\n' "$out"
    fi

    out=$({ printf 'sleep 30\n'; sleep 0.3; printf '\003echo alive\nexit\n'; } | HOME="$tmp/home" script -qec "./ash" /dev/null 2>/dev/null)
    if [[ "$out" == *alive* ]]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        printf 'FAIL: ctrl_c_foreground\n  actual: %q\n' "$out"
    fi

    out=$({ printf 'sleep 30\n'; sleep 0.3; printf '\032'; sleep 0.3; printf 'jobs\nkill -9 %%1\nwait\necho done\nexit\n'; } | HOME="$tmp/home" script -qec "./ash" /dev/null 2>/dev/null)
    if [[ "$out" == *Stopped* && "$out" == *done* ]]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        printf 'FAIL: ctrl_z_jobs_kill\n  actual: %q\n' "$out"
    fi

    out=$(printf 'ech\t\nexit\n' | HOME="$tmp/home" script -qec "./ash" /dev/null 2>/dev/null)
    if [[ "$out" == *echo* && "$out" != *"not found"* ]]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        printf 'FAIL: tab_completion\n  actual: %q\n' "$out"
    fi

    out=$(printf 'echo first\n\033[A\nexit\n' | HOME="$tmp/home" script -qec "./ash" /dev/null 2>/dev/null)
    count=$(grep -c first <<< "$out")
    if [[ "$count" -ge 2 ]]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        printf 'FAIL: history_up_arrow\n  actual: %q\n' "$out"
    fi

    mkdir -p "$tmp/home_rps"
    printf "RPS1='[RPS]'\n" > "$tmp/home_rps/.ashrc"
    out=$(printf 'exit\n' | HOME="$tmp/home_rps" script -qec "./ash" /dev/null 2>/dev/null)
    if [[ "$out" == *"[RPS]"* ]]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        printf 'FAIL: rps1_right_prompt\n  actual: %q\n' "$out"
    fi

    out=$(printf 'echo hi\nexit\n' | HOME="$tmp/home" script -qec "./ash" /dev/null 2>/dev/null)
    if [[ "$out" == *$'\x1b[32mecho'* ]]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        printf 'FAIL: syntax_highlight_command\n  actual: %q\n' "$out"
    fi

    out=$(printf 'zzz\nexit\n' | HOME="$tmp/home" script -qec "./ash" /dev/null 2>/dev/null)
    if [[ "$out" == *$'\x1b[31mzzz'* ]]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        printf 'FAIL: syntax_highlight_unknown\n  actual: %q\n' "$out"
    fi

    out=$(printf 'junk\025echo $((6*7))\nexit\n' | HOME="$tmp/home" script -qec "./ash" /dev/null 2>/dev/null)
    if [[ "$out" == *42* ]]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        printf 'FAIL: ctrl_u_kill_line\n  actual: %q\n' "$out"
    fi

    mkdir -p "$tmp/home_hist"
    printf '%s\n' 'echo $((6*7))' 'exit' | HOME="$tmp/home_hist" script -qec "./ash" /dev/null >/dev/null 2>&1
    out=$({ sleep 0.3; printf '\0226\n'; } | HOME="$tmp/home_hist" script -qec "./ash" /dev/null 2>/dev/null)
    if [[ "$out" == *42* ]]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        printf 'FAIL: ctrl_r_history_search\n  actual: %q\n' "$out"
    fi

    out=$(printf 'echo histentry\nexit\n' | HOME="$tmp/home_hist" script -qec "./ash" /dev/null >/dev/null 2>&1)
    out=$(printf 'history\nexit\n' | HOME="$tmp/home_hist" script -qec "./ash" /dev/null 2>/dev/null)
    if [[ "$out" == *histentry* ]]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        printf 'FAIL: history_builtin\n  actual: %q\n' "$out"
    fi

    mkdir -p "$tmp/home_color"
    printf '%s\n' "PS1='%F{green}P%f '" > "$tmp/home_color/.ashrc"
    out=$(printf 'exit\n' | HOME="$tmp/home_color" script -qec "./ash" /dev/null 2>/dev/null)
    if [[ "$out" == *$'\x1b[32mP'* ]]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        printf 'FAIL: prompt_color\n  actual: %q\n' "$out"
    fi

    mkdir -p "$tmp/repo/.git" "$tmp/home_git"
    printf 'ref: refs/heads/trunk\n' > "$tmp/repo/.git/HEAD"
    printf '%s\n' "PS1='%g '" > "$tmp/home_git/.ashrc"
    out=$(printf 'cd %s\nexit\n' "$tmp/repo" | HOME="$tmp/home_git" script -qec "./ash" /dev/null 2>/dev/null)
    if [[ "$out" == *"(trunk)"* ]]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        printf 'FAIL: prompt_git_branch\n  actual: %q\n' "$out"
    fi

    mkdir -p "$tmp/home_cond"
    printf '%s\n' "RPS1='%(?..FAIL)'" > "$tmp/home_cond/.ashrc"
    out=$(printf 'false\nexit\n' | HOME="$tmp/home_cond" script -qec "./ash" /dev/null 2>/dev/null)
    if [[ "$out" == *FAIL* ]]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        printf 'FAIL: prompt_conditional_fail\n  actual: %q\n' "$out"
    fi
    out=$(printf 'true\nexit\n' | HOME="$tmp/home_cond" script -qec "./ash" /dev/null 2>/dev/null)
    if [[ "$out" != *FAIL* ]]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        printf 'FAIL: prompt_conditional_ok\n  actual: %q\n' "$out"
    fi

    mkdir -p "$tmp/home_bg"
    printf '%s\n' "PS1='%K{blue}B%k '" > "$tmp/home_bg/.ashrc"
    out=$(printf 'exit\n' | HOME="$tmp/home_bg" script -qec "./ash" /dev/null 2>/dev/null)
    if [[ "$out" == *$'\x1b[44mB'* && "$out" == *$'\x1b[49m'* ]]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        printf 'FAIL: prompt_bg_color\n  actual: %q\n' "$out"
    fi

    mkdir -p "$tmp/home_plain"
    out=$(printf 'exit\n' | HOME="$tmp/home_plain" script -qec "./ash" /dev/null 2>/dev/null)
    if [[ "$out" == *$'\u276f'* && "$out" == *$'\ue0b0'* ]]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        printf 'FAIL: prompt_default_glyph\n  actual: %q\n' "$out"
    fi

    mkdir -p "$tmp/home_tr"
    out=$(printf 'echo $((6*7))\n/bin/echo $((8*8))\nexit\n' | HOME="$tmp/home_tr" script -qec "./ash" /dev/null 2>/dev/null)
    tr="$tmp/home_tr/.ash_transcript"
    if [[ -f "$tr" ]] && grep -q 42 "$tr" && grep -q 64 "$tr" \
        && ! grep -q 42 "$tmp/home_tr/.ash_history" \
        && [[ "$(stat -c %a "$tr")" == 600 ]]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        printf 'FAIL: transcript_logs_output\n  actual: %q\n' "$out"
    fi

    mkdir -p "$tmp/home_tr2"
    printf '%s\n' 'setopt notranscript' > "$tmp/home_tr2/.ashrc"
    out=$(printf 'exit\n' | HOME="$tmp/home_tr2" script -qec "./ash" /dev/null 2>/dev/null)
    if [[ ! -e "$tmp/home_tr2/.ash_transcript" ]]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        printf 'FAIL: transcript_opt_out\n  actual: %q\n' "$out"
    fi

    mkdir -p "$tmp/home_hp"
    printf 'old_line\n' > "$tmp/home_hp/.ash_history"
    chmod 644 "$tmp/home_hp/.ash_history"
    out=$(printf 'echo hp\n' | HOME="$tmp/home_hp" script -qec "./ash" /dev/null 2>/dev/null)
    hp="$tmp/home_hp/.ash_history"
    if [[ "$(stat -c %a "$hp")" == 600 ]] && grep -q old_line "$hp" \
        && grep -q 'echo hp' "$hp"; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        printf 'FAIL: history_permissions\n  actual: %q\n' "$out"
    fi
else
    printf 'SKIP: interactive tests (no script command)\n'
fi

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[[ $fail -eq 0 ]]
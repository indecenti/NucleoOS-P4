// nv_js.c — `js`, QuickJS-ng for the NucleoOS terminal.
//
//   js                    interactive prompt (line-based: the terminal sends whole lines)
//   js file.js [args]     run a script (ES module when it uses import/export or ends in .mjs)
//   js -e "code"          evaluate one expression and exit
//
// Upstream qjs's REPL (repl.js) drives a raw-mode tty through os.setReadHandler + poll(), which
// the device's WASI layer doesn't provide, so this prompt is a plain read-eval-print loop:
// unbalanced brackets continue on the next line, results are printed with a small inspector,
// errors go to stderr. std, os and bjson are globals as with `qjs --std`, and so are the
// browser-style timers (setTimeout & co.), which run to completion before the next prompt.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "quickjs.h"
#include "quickjs-libc.h"

#define JS_MEM_LIMIT   (6u * 1024 * 1024)   // leave headroom in the app's 8 MB linear memory
#define JS_STACK_LIMIT (200u * 1024)        // < the 256 KB C stack the module is linked with

static const char kPrelude[] =
    "import * as bjson from 'qjs:bjson';\n"
    "import * as std from 'qjs:std';\n"
    "import * as os from 'qjs:os';\n"
    "globalThis.bjson = bjson;\n"
    "globalThis.std = std;\n"
    "globalThis.os = os;\n"
    "globalThis.setTimeout = os.setTimeout;\n"
    "globalThis.clearTimeout = os.clearTimeout;\n"
    "globalThis.setInterval = os.setInterval;\n"
    "globalThis.clearInterval = os.clearInterval;\n";

// REPL result printer: strings quoted at depth > 0, arrays/objects expanded two levels deep.
static const char kInspect[] =
    "(function () {\n"
    "  function q(s) { return JSON.stringify(s); }\n"
    "  function fmt(v, d, seen) {\n"
    "    switch (typeof v) {\n"
    "    case 'string': return d ? q(v) : v;\n"
    "    case 'bigint': return v + 'n';\n"
    "    case 'symbol': return v.toString();\n"
    "    case 'function': return '[Function' + (v.name ? ': ' + v.name : ' (anonymous)') + ']';\n"
    "    case 'object': break;\n"
    "    default: return String(v);\n"
    "    }\n"
    "    if (v === null) return 'null';\n"
    "    if (seen.indexOf(v) >= 0) return '[Circular]';\n"
    "    if (v instanceof Error) return v.name + ': ' + v.message;\n"
    "    if (v instanceof Date) return v.toISOString();\n"
    "    if (v instanceof RegExp) return v.toString();\n"
    "    if (d > 2) return Array.isArray(v) ? '[Array]' : '[Object]';\n"
    "    seen.push(v);\n"
    "    var out;\n"
    "    if (Array.isArray(v)) {\n"
    "      var a = [], n = Math.min(v.length, 100);\n"
    "      for (var i = 0; i < n; i++) a.push(fmt(v[i], d + 1, seen));\n"
    "      if (v.length > n) a.push('... ' + (v.length - n) + ' more');\n"
    "      out = '[ ' + a.join(', ') + ' ]';\n"
    "      if (a.length === 0) out = '[]';\n"
    "    } else if (v instanceof Map || v instanceof Set) {\n"
    "      var e = [];\n"
    "      v.forEach(function (val, k) {\n"
    "        e.push(v instanceof Map ? fmt(k, d + 1, seen) + ' => ' + fmt(val, d + 1, seen)\n"
    "                                : fmt(val, d + 1, seen));\n"
    "      });\n"
    "      out = (v instanceof Map ? 'Map' : 'Set') + '(' + v.size + ') { ' + e.join(', ') + ' }';\n"
    "    } else {\n"
    "      var ks = Object.keys(v), p = [];\n"
    "      for (var j = 0; j < ks.length && j < 100; j++)\n"
    "        p.push((/^[A-Za-z_$][\\w$]*$/.test(ks[j]) ? ks[j] : q(ks[j])) + ': ' +\n"
    "               fmt(v[ks[j]], d + 1, seen));\n"
    "      var ctor = v.constructor && v.constructor.name;\n"
    "      var pre = ctor && ctor !== 'Object' ? ctor + ' ' : '';\n"
    "      out = p.length ? pre + '{ ' + p.join(', ') + ' }' : pre + '{}';\n"
    "    }\n"
    "    seen.pop();\n"
    "    return out;\n"
    "  }\n"
    "  globalThis.__nv_inspect = function (v) { return fmt(v, 0, []); };\n"
    "})();\n";

static JSContext *new_context(JSRuntime *rt) {
    JSContext *ctx = JS_NewContext(rt);
    if (!ctx) return NULL;
    js_init_module_std(ctx, "qjs:std");
    js_init_module_os(ctx, "qjs:os");
    js_init_module_bjson(ctx, "qjs:bjson");
    return ctx;
}

// Runs what the last evaluation queued — promise jobs and timers — before the next prompt. A
// setInterval that is never cleared keeps the prompt busy until the program is stopped.
static void run_jobs(JSContext *ctx) {
    if (js_std_loop(ctx)) js_std_dump_error(ctx);
}

static int eval_buf(JSContext *ctx, const char *buf, size_t len, const char *name, int flags,
                    int print) {
    JSValue val;
    if ((flags & JS_EVAL_TYPE_MASK) == JS_EVAL_TYPE_MODULE) {
        // Compile first so import.meta is set, then run: a module evaluates to a promise.
        val = JS_Eval(ctx, buf, len, name, flags | JS_EVAL_FLAG_COMPILE_ONLY);
        if (!JS_IsException(val)) {
            if (js_module_set_import_meta(ctx, val, name[0] != '<', true) < 0) {
                JS_FreeValue(ctx, val);
                val = JS_EXCEPTION;
            } else {
                val = JS_EvalFunction(ctx, val);
            }
        }
        if (!JS_IsException(val)) val = js_std_await(ctx, val);
    } else {
        val = JS_Eval(ctx, buf, len, name, flags);
    }
    if (JS_IsException(val)) {
        js_std_dump_error(ctx);
        return -1;
    }
    if (print && !JS_IsUndefined(val)) {
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue fn = JS_GetPropertyStr(ctx, global, "__nv_inspect");
        JSValue s = JS_Call(ctx, fn, JS_UNDEFINED, 1, (JSValueConst *)&val);
        if (JS_IsException(s)) {
            js_std_dump_error(ctx);
        } else {
            const char *str = JS_ToCString(ctx, s);
            if (str) {
                puts(str);
                JS_FreeCString(ctx, str);
            }
        }
        JS_FreeValue(ctx, s);
        JS_FreeValue(ctx, fn);
        JS_FreeValue(ctx, global);
    }
    JS_FreeValue(ctx, val);
    return 0;
}

// Bracket depth of `src`, ignoring strings, template literals and comments. > 0 means the
// statement continues on the next line.
static int open_depth(const char *src) {
    int depth = 0;
    char quote = 0;
    for (const char *p = src; *p; p++) {
        if (quote) {
            if (*p == '\\' && p[1]) p++;
            else if (*p == quote) quote = 0;
            continue;
        }
        if (p[0] == '/' && p[1] == '/') {
            while (*p && *p != '\n') p++;
            if (!*p) break;
            continue;
        }
        if (p[0] == '/' && p[1] == '*') {
            const char *e = strstr(p + 2, "*/");
            if (!e) return depth + 1;   // unterminated comment: keep reading
            p = e + 1;
            continue;
        }
        switch (*p) {
        case '"': case '\'': case '`': quote = *p; break;
        case '(': case '[': case '{': depth++; break;
        case ')': case ']': case '}': depth--; break;
        }
    }
    return quote == '`' ? depth + 1 : depth;
}

static void repl(JSContext *ctx) {
    size_t cap = 4096, len = 0;
    char *src = malloc(cap);
    char line[1024];
    if (!src) return;
    src[0] = '\0';
    printf("QuickJS-ng %s  (std, os, bjson loaded; Ctrl-D or EOF to quit)\n", JS_GetVersion());
    for (;;) {
        fputs(len ? "... " : "> ", stdout);
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin)) break;
        const size_t n = strlen(line);
        if (len + n + 1 > cap) {
            char *g = realloc(src, cap = (len + n + 1) * 2);
            if (!g) break;
            src = g;
        }
        memcpy(src + len, line, n + 1);
        len += n;
        if (open_depth(src) > 0) continue;
        if (strspn(src, " \t\r\n") != len) {
            eval_buf(ctx, src, len, "<repl>", JS_EVAL_TYPE_GLOBAL, 1);
            run_jobs(ctx);
        }
        len = 0;
        src[0] = '\0';
        fflush(stdout);
    }
    putchar('\n');
    free(src);
}

static int run_file(JSContext *ctx, const char *path, int force_module) {
    size_t len;
    uint8_t *buf = js_load_file(ctx, &len, path);
    if (!buf) {
        fprintf(stderr, "js: cannot open %s\n", path);
        return -1;
    }
    const size_t pl = strlen(path);
    const int module = force_module || (pl > 4 && !strcmp(path + pl - 4, ".mjs")) ||
                       JS_DetectModule((const char *)buf, len);
    const int r = eval_buf(ctx, (const char *)buf, len, path,
                           module ? JS_EVAL_TYPE_MODULE : JS_EVAL_TYPE_GLOBAL, 0);
    js_free(ctx, buf);
    return r;
}

static void usage(void) {
    puts("usage: js [-m] [file.js [args...]]   run a script (-m: as an ES module)\n"
         "       js -e \"code\"                  evaluate code\n"
         "       js                            interactive prompt");
}

int main(int argc, char **argv) {
    const char *expr = NULL;
    int module = 0, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "-e") && i + 1 < argc) expr = argv[++i];
        else if (!strcmp(argv[i], "-m")) module = 1;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(); return 0; }
        else { usage(); return 2; }
    }

    JSRuntime *rt = JS_NewRuntime();
    if (!rt) { fputs("js: cannot allocate the runtime\n", stderr); return 1; }
    JS_SetMemoryLimit(rt, JS_MEM_LIMIT);
    JS_SetMaxStackSize(rt, JS_STACK_LIMIT);
    js_std_set_worker_new_context_func(new_context);
    js_std_init_handlers(rt);
    JS_SetModuleLoaderFunc2(rt, NULL, js_module_loader, js_module_check_attributes, NULL);
    JSContext *ctx = new_context(rt);
    if (!ctx) { fputs("js: cannot allocate the context\n", stderr); return 1; }

    js_std_add_helpers(ctx, argc - i, argv + i);
    eval_buf(ctx, kPrelude, sizeof kPrelude - 1, "<prelude>", JS_EVAL_TYPE_MODULE, 0);

    int rc = 0;
    if (expr) {
        rc = eval_buf(ctx, expr, strlen(expr), "<cmdline>",
                      module ? JS_EVAL_TYPE_MODULE : JS_EVAL_TYPE_GLOBAL, 0) ? 1 : 0;
        if (!rc) rc = js_std_loop(ctx) ? 1 : 0;
    } else if (i < argc) {
        rc = run_file(ctx, argv[i], module) ? 1 : 0;
        if (!rc && js_std_loop(ctx)) {
            js_std_dump_error(ctx);
            rc = 1;
        }
    } else {
        JS_SetHostPromiseRejectionTracker(rt, NULL, NULL);
        eval_buf(ctx, kInspect, sizeof kInspect - 1, "<inspect>", JS_EVAL_TYPE_GLOBAL, 0);
        repl(ctx);
    }
    fflush(stdout);
    js_std_free_handlers(rt);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return rc;
}

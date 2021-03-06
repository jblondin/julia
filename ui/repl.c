// This file is a part of Julia. License is MIT: http://julialang.org/license

/*
  repl.c
  system startup, main(), and console interaction
*/

#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <setjmp.h>
#include <signal.h>
#include <assert.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <limits.h>
#include <errno.h>
#include <math.h>
#include <ctype.h>

#ifndef _MSC_VER
#include <unistd.h>
#include <getopt.h>
#else
#include "getopt.h"
#endif

#include "uv.h"
#define WHOLE_ARCHIVE
#include "../src/julia.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef JULIA_ENABLE_THREADING
static JL_CONST_FUNC jl_tls_states_t *jl_get_ptls_states_static(void)
{
#  if !defined(_COMPILER_MICROSOFT_)
    static __attribute__((tls_model("local-exec"))) __thread jl_tls_states_t tls_states;
#  else
    static __declspec(thread) jl_tls_states_t tls_states;
#  endif
    return &tls_states;
}
#endif

static int lisp_prompt = 0;
static int codecov  = JL_LOG_NONE;
static int malloclog= JL_LOG_NONE;
static int imagepathspecified = 0;

static const char usage[] = "julia [switches] -- [programfile] [args...]\n";
static const char opts[]  =
    " -v, --version             Display version information\n"
    " -h, --help                Print this message\n\n"

    // startup options
    " -J, --sysimage <file>     Start up with the given system image file\n"
    " --precompiled={yes|no}    Use precompiled code from system image if available\n"
    " --compilecache={yes|no}   Enable/disable incremental precompilation of modules\n"
    " -H, --home <dir>          Set location of julia executable\n"
    " --startup-file={yes|no}   Load ~/.juliarc.jl\n"
    " -f, --no-startup          Don't load ~/.juliarc (deprecated, use --startup-file=no)\n"
    " -F                        Load ~/.juliarc (deprecated, use --startup-file=yes)\n"
    " --handle-signals={yes|no} Enable or disable Julia's default signal handlers\n\n"

    // actions
    " -e, --eval <expr>         Evaluate <expr>\n"
    " -E, --print <expr>        Evaluate and show <expr>\n"
    " -P, --post-boot <expr>    Evaluate <expr>, but don't disable interactive mode (deprecated, use -i -e instead)\n"
    " -L, --load <file>         Load <file> immediately on all processors\n\n"

    // parallel options
    " -p, --procs {N|auto}      Integer value N launches N additional local worker processes\n"
    "                           \"auto\" launches as many workers as the number of local cores\n"
    " --machinefile <file>      Run processes on hosts listed in <file>\n\n"

    // interactive options
    " -i                        Interactive mode; REPL runs and isinteractive() is true\n"
    " -q, --quiet               Quiet startup (no banner)\n"
    " --color={yes|no}          Enable or disable color text\n"
    " --history-file={yes|no}   Load or save history\n"
    " --no-history-file         Don't load history file (deprecated, use --history-file=no)\n\n"

    // code generation options
    " --compile={yes|no|all|min}Enable or disable JIT compiler, or request exhaustive compilation\n"
    " -C, --cpu-target <target> Limit usage of cpu features up to <target>\n"
    " -O, --optimize={0,1,2,3}  Set the optimization level (default 2 if unspecified or 3 if specified as -O)\n"
    " --inline={yes|no}         Control whether inlining is permitted (overrides functions declared as @inline)\n"
    " --check-bounds={yes|no}   Emit bounds checks always or never (ignoring declarations)\n"
    " --math-mode={ieee,fast}   Disallow or enable unsafe floating point optimizations (overrides @fastmath declaration)\n\n"

    // error and warning options
    " --depwarn={yes|no|error}  Enable or disable syntax and method deprecation warnings (\"error\" turns warnings into errors)\n\n"

    // compiler output options
    " --output-o name           Generate an object file (including system image data)\n"
    " --output-ji name          Generate a system image data file (.ji)\n"
    " --output-bc name          Generate LLVM bitcode (.bc)\n\n"
    " --output-incremental=no   Generate an incremental output file (rather than complete)\n\n"

    // instrumentation options
    " --code-coverage={none|user|all}, --code-coverage\n"
    "                           Count executions of source lines (omitting setting is equivalent to \"user\")\n"
    " --track-allocation={none|user|all}, --track-allocation\n"
    "                           Count bytes allocated by each source line\n";

void parse_opts(int *argcp, char ***argvp)
{
    enum { opt_machinefile = 300,
           opt_color,
           opt_history_file,
           opt_no_history_file,
           opt_startup_file,
           opt_compile,
           opt_code_coverage,
           opt_track_allocation,
           opt_check_bounds,
           opt_output_bc,
           opt_depwarn,
           opt_inline,
           opt_math_mode,
           opt_worker,
           opt_bind_to,
           opt_handle_signals,
           opt_output_o,
           opt_output_ji,
           opt_use_precompiled,
           opt_use_compilecache,
           opt_incremental
    };
    static char* shortopts = "+vhqFfH:e:E:P:L:J:C:ip:O:";
    static struct option longopts[] = {
        // exposed command line options
        // NOTE: This set of required arguments need to be kept in sync
        // with the required arguments defined in base/client.jl `process_options()`
        { "version",         no_argument,       0, 'v' },
        { "help",            no_argument,       0, 'h' },
        { "quiet",           no_argument,       0, 'q' },
        { "home",            required_argument, 0, 'H' },
        { "eval",            required_argument, 0, 'e' },
        { "print",           required_argument, 0, 'E' },
        { "post-boot",       required_argument, 0, 'P' },
        { "load",            required_argument, 0, 'L' },
        { "sysimage",        required_argument, 0, 'J' },
        { "precompiled",     required_argument, 0, opt_use_precompiled },
        { "compilecache",    required_argument, 0, opt_use_compilecache },
        { "cpu-target",      required_argument, 0, 'C' },
        { "procs",           required_argument, 0, 'p' },
        { "machinefile",     required_argument, 0, opt_machinefile },
        { "color",           required_argument, 0, opt_color },
        { "history-file",    required_argument, 0, opt_history_file },
        { "no-history-file", no_argument,       0, opt_no_history_file }, // deprecated
        { "startup-file",    required_argument, 0, opt_startup_file },
        { "no-startup",      no_argument,       0, 'f' },                 // deprecated
        { "compile",         required_argument, 0, opt_compile },
        { "code-coverage",   optional_argument, 0, opt_code_coverage },
        { "track-allocation",optional_argument, 0, opt_track_allocation },
        { "optimize",        optional_argument, 0, 'O' },
        { "check-bounds",    required_argument, 0, opt_check_bounds },
        { "output-bc",       required_argument, 0, opt_output_bc },
        { "output-o",        required_argument, 0, opt_output_o },
        { "output-ji",       required_argument, 0, opt_output_ji },
        { "output-incremental",required_argument, 0, opt_incremental },
        { "depwarn",         required_argument, 0, opt_depwarn },
        { "inline",          required_argument, 0, opt_inline },
        { "math-mode",       required_argument, 0, opt_math_mode },
        { "handle-signals",  required_argument, 0, opt_handle_signals },
        // hidden command line options
        { "worker",          required_argument, 0, opt_worker },
        { "bind-to",         required_argument, 0, opt_bind_to },
        { "lisp",            no_argument,       &lisp_prompt, 1 },
        { 0, 0, 0, 0 }
    };
    // getopt handles argument parsing up to -- delineator
    int argc = *argcp;
    char **argv = *argvp;
    if (argc > 0) {
        for (int i=0; i < argc; i++) {
            if (!strcmp(argv[i], "--")) {
                argc = i;
                break;
            }
        }
    }
    char *endptr;
    opterr = 0; // suppress getopt warning messages
    while (1) {
        int lastind = optind;
        int c = getopt_long(argc, argv, shortopts, longopts, 0);
        if (c == -1) break;
restart_switch:
        switch (c) {
        case 0:
            break;
        case '?':
        case ':':
            if (optopt) {
                for (struct option *o = longopts; o->val; o++) {
                    if (optopt == o->val) {
                        if (o->has_arg == optional_argument) {
                            c = o->val;
                            goto restart_switch;
                        }
                        else if (strchr(shortopts, o->val)) {
                            jl_errorf("option `-%c/--%s` is missing an argument", o->val, o->name);
                        }
                        else {
                            jl_errorf("option `--%s` is missing an argument", o->name);
                        }
                    }
                }
                jl_errorf("unknown option `-%c`", optopt);
            } else {
                jl_errorf("unknown option `%s`", argv[lastind]);
            }
            break;
        case 'v': // version
            jl_printf(JL_STDOUT, "julia version %s\n", JULIA_VERSION_STRING);
            jl_exit(0);
        case 'h': // help
            jl_printf(JL_STDOUT, "%s%s", usage, opts);
            jl_exit(0);
        case 'q': // quiet
            jl_options.quiet = 1;
            break;
        case 'H': // home
            jl_options.julia_home = strdup(optarg);
            break;
        case 'e': // eval
            jl_options.eval = strdup(optarg);
            break;
        case 'E': // print
            jl_options.print = strdup(optarg);
            break;
        case 'P': // post-boot
            jl_options.postboot = strdup(optarg);
            break;
        case 'L': // load
            jl_options.load = strdup(optarg);
            break;
        case 'J': // sysimage
            jl_options.image_file = strdup(optarg);
            imagepathspecified = 1;
            break;
        case opt_use_precompiled:
            if (!strcmp(optarg,"yes"))
                jl_options.use_precompiled = JL_OPTIONS_USE_PRECOMPILED_YES;
            else if (!strcmp(optarg,"no"))
                jl_options.use_precompiled = JL_OPTIONS_USE_PRECOMPILED_NO;
            else
                jl_errorf("julia: invalid argument to --precompiled={yes|no} (%s)", optarg);
            break;
        case opt_use_compilecache:
            if (!strcmp(optarg,"yes"))
                jl_options.use_compilecache = JL_OPTIONS_USE_COMPILECACHE_YES;
            else if (!strcmp(optarg,"no"))
                jl_options.use_compilecache = JL_OPTIONS_USE_COMPILECACHE_NO;
            else
                jl_errorf("julia: invalid argument to --compilecache={yes|no} (%s)", optarg);
            break;
        case 'C': // cpu-target
            jl_options.cpu_target = strdup(optarg);
            break;
        case 'p': // procs
            errno = 0;
            if (!strcmp(optarg,"auto")) {
                jl_options.nprocs = jl_cpu_cores();
            }
            else {
                long nprocs = strtol(optarg, &endptr, 10);
                if (errno != 0 || optarg == endptr || *endptr != 0 || nprocs < 1 || nprocs >= INT_MAX)
                    jl_errorf("julia: -p,--procs=<n> must be an integer >= 1");
                jl_options.nprocs = (int)nprocs;
            }
            break;
        case opt_machinefile:
            jl_options.machinefile = strdup(optarg);
            break;
        case opt_color:
            if (!strcmp(optarg,"yes"))
                jl_options.color = JL_OPTIONS_COLOR_ON;
            else if (!strcmp(optarg,"no"))
                jl_options.color = JL_OPTIONS_COLOR_OFF;
            else
                jl_errorf("julia: invalid argument to --color={yes|no} (%s)", optarg);
            break;
        case opt_history_file:
            if (!strcmp(optarg,"yes"))
                jl_options.historyfile = JL_OPTIONS_HISTORYFILE_ON;
            else if (!strcmp(optarg,"no"))
                jl_options.historyfile = JL_OPTIONS_HISTORYFILE_OFF;
            else
                jl_errorf("julia: invalid argument to --history-file={yes|no} (%s)", optarg);
            break;
        case opt_no_history_file:
            jl_options.historyfile = JL_OPTIONS_HISTORYFILE_OFF;
            break;
        case opt_startup_file:
            if (!strcmp(optarg,"yes"))
                jl_options.startupfile = JL_OPTIONS_STARTUPFILE_ON;
            else if (!strcmp(optarg,"no"))
                jl_options.startupfile = JL_OPTIONS_STARTUPFILE_OFF;
            else
                jl_errorf("julia: invalid argument to --startup-file={yes|no} (%s)", optarg);
            break;
        case 'f':
            jl_options.startupfile = JL_OPTIONS_STARTUPFILE_OFF;
            break;
        case 'F':
            jl_options.startupfile = JL_OPTIONS_STARTUPFILE_ON;
            break;
        case opt_compile:
            if (!strcmp(optarg,"yes"))
                jl_options.compile_enabled = JL_OPTIONS_COMPILE_ON;
            else if (!strcmp(optarg,"no"))
                jl_options.compile_enabled = JL_OPTIONS_COMPILE_OFF;
            else if (!strcmp(optarg,"all"))
                jl_options.compile_enabled = JL_OPTIONS_COMPILE_ALL;
            else if (!strcmp(optarg,"min"))
                jl_options.compile_enabled = JL_OPTIONS_COMPILE_MIN;
            else
                jl_errorf("julia: invalid argument to --compile (%s)", optarg);
            break;
        case opt_code_coverage:
            if (optarg != NULL) {
                if (!strcmp(optarg,"user"))
                    codecov = JL_LOG_USER;
                else if (!strcmp(optarg,"all"))
                    codecov = JL_LOG_ALL;
                else if (!strcmp(optarg,"none"))
                    codecov = JL_LOG_NONE;
                else
                    jl_errorf("julia: invalid argument to --code-coverage (%s)", optarg);
                break;
            }
            else {
                codecov = JL_LOG_USER;
            }
            break;
        case opt_track_allocation:
            if (optarg != NULL) {
                if (!strcmp(optarg,"user"))
                    malloclog = JL_LOG_USER;
                else if (!strcmp(optarg,"all"))
                    malloclog = JL_LOG_ALL;
                else if (!strcmp(optarg,"none"))
                    malloclog = JL_LOG_NONE;
                else
                    jl_errorf("julia: invalid argument to --track-allocation (%s)", optarg);
                break;
            }
            else {
                malloclog = JL_LOG_USER;
            }
            break;
        case 'O': // optimize
            if (optarg != NULL) {
                if (!strcmp(optarg,"0"))
                    jl_options.opt_level = 0;
                else if (!strcmp(optarg,"1"))
                    jl_options.opt_level = 1;
                else if (!strcmp(optarg,"2"))
                    jl_options.opt_level = 2;
                else if (!strcmp(optarg,"3"))
                    jl_options.opt_level = 3;
                else
                    jl_errorf("julia: invalid argument to -O (%s)", optarg);
                break;
            }
            else {
                jl_options.opt_level = 3;
            }
            break;
        case 'i': // isinteractive
            jl_options.isinteractive = 1;
            break;
        case opt_check_bounds:
            if (!strcmp(optarg,"yes"))
                jl_options.check_bounds = JL_OPTIONS_CHECK_BOUNDS_ON;
            else if (!strcmp(optarg,"no"))
                jl_options.check_bounds = JL_OPTIONS_CHECK_BOUNDS_OFF;
            else
                jl_errorf("julia: invalid argument to --check-bounds={yes|no} (%s)", optarg);
            break;
        case opt_output_bc:
            jl_options.outputbc = optarg;
            if (!imagepathspecified) jl_options.image_file = NULL;
            break;
        case opt_output_o:
            jl_options.outputo = optarg;
            if (!imagepathspecified) jl_options.image_file = NULL;
            break;
        case opt_output_ji:
            jl_options.outputji = optarg;
            if (!imagepathspecified) jl_options.image_file = NULL;
            break;
        case opt_incremental:
            if (!strcmp(optarg,"yes"))
                jl_options.incremental = 1;
            else if (!strcmp(optarg,"no"))
                jl_options.incremental = 0;
            else
                jl_errorf("julia: invalid argument to --output-incremental={yes|no} (%s)", optarg);
            break;
        case opt_depwarn:
            if (!strcmp(optarg,"yes"))
                jl_options.depwarn = JL_OPTIONS_DEPWARN_ON;
            else if (!strcmp(optarg,"no"))
                jl_options.depwarn = JL_OPTIONS_DEPWARN_OFF;
            else if (!strcmp(optarg,"error"))
                jl_options.depwarn = JL_OPTIONS_DEPWARN_ERROR;
            else
                jl_errorf("julia: invalid argument to --depwarn={yes|no|error} (%s)", optarg);
            break;
        case opt_inline:
            if (!strcmp(optarg,"yes"))
                jl_options.can_inline = 1;
            else if (!strcmp(optarg,"no"))
                jl_options.can_inline = 0;
            else {
                jl_errorf("julia: invalid argument to --inline (%s)", optarg);
            }
            break;
        case opt_math_mode:
            if (!strcmp(optarg,"ieee"))
                jl_options.fast_math = JL_OPTIONS_FAST_MATH_OFF;
            else if (!strcmp(optarg,"fast"))
                jl_options.fast_math = JL_OPTIONS_FAST_MATH_ON;
            else if (!strcmp(optarg,"user"))
                jl_options.fast_math = JL_OPTIONS_FAST_MATH_DEFAULT;
            else
                jl_errorf("julia: invalid argument to --math-mode (%s)", optarg);
            break;
        case opt_worker:
            jl_options.worker = strdup(optarg);
            break;
        case opt_bind_to:
            jl_options.bindto = strdup(optarg);
            break;
        case opt_handle_signals:
            if (!strcmp(optarg,"yes"))
                jl_options.handle_signals = JL_OPTIONS_HANDLE_SIGNALS_ON;
            else if (!strcmp(optarg,"no"))
                jl_options.handle_signals = JL_OPTIONS_HANDLE_SIGNALS_OFF;
            else
                jl_errorf("julia: invalid argument to --handle-signals (%s)", optarg);
            break;
        default:
            jl_errorf("julia: unhandled option -- %c\n"
                      "This is a bug, please report it.", c);
        }
    }
    jl_options.code_coverage = codecov;
    jl_options.malloc_log = malloclog;
    *argvp += optind;
    *argcp -= optind;
}

static int exec_program(char *program)
{
    int err = 0;
 again: ;
    JL_TRY {
        if (err) {
            jl_value_t *errs = jl_stderr_obj();
            jl_value_t *e = jl_exception_in_transit;
            if (errs != NULL) {
                jl_show(errs, e);
            }
            else {
                jl_printf(JL_STDERR, "error during bootstrap:\n");
                jl_static_show(JL_STDERR, e);
                jl_printf(JL_STDERR, "\n");
                jlbacktrace();
            }
            jl_printf(JL_STDERR, "\n");
            JL_EH_POP();
            return 1;
        }
        jl_load(program, strlen(program));
    }
    JL_CATCH {
        err = 1;
        goto again;
    }
    return 0;
}

void jl_lisp_prompt();

#ifndef _WIN32
int jl_repl_raise_sigtstp(void)
{
    return raise(SIGTSTP);
}
#endif

#ifdef JL_GF_PROFILE
static void print_profile(void)
{
    size_t i;
    void **table = jl_base_module->bindings.table;
    for(i=1; i < jl_base_module->bindings.size; i+=2) {
        if (table[i] != HT_NOTFOUND) {
            jl_binding_t *b = (jl_binding_t*)table[i];
            if (b->value != NULL && jl_is_function(b->value) &&
                jl_is_gf(b->value)) {
                jl_printf(JL_STDERR, "%d\t%s\n",
                           jl_gf_mtable(b->value)->ncalls,
                           jl_gf_name(b->value)->name);
            }
        }
    }
}
#endif

static NOINLINE int true_main(int argc, char *argv[])
{
    if (jl_core_module != NULL) {
        jl_array_t *args = (jl_array_t*)jl_get_global(jl_core_module, jl_symbol("ARGS"));
        if (args == NULL) {
            args = jl_alloc_cell_1d(0);
            JL_GC_PUSH1(&args);
            jl_set_const(jl_core_module, jl_symbol("ARGS"), (jl_value_t*)args);
            JL_GC_POP();
        }
        assert(jl_array_len(args) == 0);
        jl_array_grow_end(args, argc);
        int i;
        for (i=0; i < argc; i++) {
            jl_value_t *s = (jl_value_t*)jl_cstr_to_string(argv[i]);
            jl_arrayset(args, s, i);
        }
    }

    jl_function_t *start_client = jl_base_module ?
        (jl_function_t*)jl_get_global(jl_base_module, jl_symbol("_start")) : NULL;

    if (start_client) {
        jl_apply(&start_client, 1);
        return 0;
    }

    // run program if specified, otherwise enter REPL
    if (argc > 0) {
        if (strcmp(argv[0], "-")) {
            return exec_program(argv[0]);
        }
    }

    ios_puts("WARNING: Base._start not defined, falling back to economy mode repl.\n", ios_stdout);
    if (!jl_errorexception_type)
        ios_puts("WARNING: jl_errorexception_type not defined; any errors will be fatal.\n", ios_stdout);

    while (!ios_eof(ios_stdin)) {
        char *volatile line = NULL;
        JL_TRY {
            ios_puts("\njulia> ", ios_stdout);
            ios_flush(ios_stdout);
            line = ios_readline(ios_stdin);
            jl_value_t *val = (jl_value_t*)jl_eval_string(line);
            if (jl_exception_occurred()) {
                jl_printf(JL_STDERR, "error during run:\n");
                jl_static_show(JL_STDERR, jl_exception_in_transit);
                jl_exception_clear();
            }
            else if (val) {
                jl_static_show(JL_STDOUT, val);
            }
            jl_printf(JL_STDOUT, "\n");
            free(line);
            line = NULL;
            uv_run(jl_global_event_loop(),UV_RUN_NOWAIT);
        }
        JL_CATCH {
            if (line) {
                free(line);
                line = NULL;
            }
            jl_printf(JL_STDERR, "\nparser error:\n");
            jl_static_show(JL_STDERR, jl_exception_in_transit);
            jl_printf(JL_STDERR, "\n");
            jlbacktrace();
        }
    }
    return 0;
}

#ifndef _OS_WINDOWS_
int main(int argc, char *argv[])
{
    uv_setup_args(argc, argv); // no-op on Windows
#else

#if defined(_P64) && defined(JL_DEBUG_BUILD)
static int is_running_under_wine()
{
    static const char * (CDECL *pwine_get_version)(void);
    HMODULE hntdll = GetModuleHandle("ntdll.dll");
    assert(hntdll);
    pwine_get_version = (void *)GetProcAddress(hntdll, "wine_get_version");
    return pwine_get_version != 0;
}
#endif

static void lock_low32() {
#if defined(_P64) && defined(JL_DEBUG_BUILD)
    // Wine currently has a that causes it to answer VirtualQuery incorrectly.
    // See https://www.winehq.org/pipermail/wine-devel/2016-March/112188.html for details
    int under_wine = is_running_under_wine();
    // block usage of the 32-bit address space on win64, to catch pointer cast errors
    char *const max32addr = (char*)0xffffffffL;
    SYSTEM_INFO info;
    MEMORY_BASIC_INFORMATION meminfo;
    GetNativeSystemInfo(&info);
    memset(&meminfo, 0, sizeof(meminfo));
    meminfo.BaseAddress = info.lpMinimumApplicationAddress;
    while ((char*)meminfo.BaseAddress < max32addr) {
        size_t nbytes = VirtualQuery(meminfo.BaseAddress, &meminfo, sizeof(meminfo));
        assert(nbytes == sizeof(meminfo));
        if (meminfo.State == MEM_FREE) { // reserve all free pages in the first 4GB of memory
            char *first = (char*)meminfo.BaseAddress;
            char *last = first + meminfo.RegionSize;
            char *p;
            if (last > max32addr)
                last = max32addr;
            // adjust first up to the first allocation granularity boundary
            // adjust last down to the last allocation granularity boundary
            first = (char*)(((long long)first + info.dwAllocationGranularity - 1) & ~(info.dwAllocationGranularity - 1));
            last = (char*)((long long)last & ~(info.dwAllocationGranularity - 1));
            if (last != first) {
                p = VirtualAlloc(first, last - first, MEM_RESERVE, PAGE_NOACCESS); // reserve all memory in between
                assert(under_wine || p == first);
            }
        }
        meminfo.BaseAddress += meminfo.RegionSize;
    }
#endif
}
int wmain(int argc, wchar_t *argv[], wchar_t *envp[])
{
    int i;
    lock_low32();
    for (i=0; i<argc; i++) { // write the command line to UTF8
        wchar_t *warg = argv[i];
        size_t len = WideCharToMultiByte(CP_UTF8, 0, warg, -1, NULL, 0, NULL, NULL);
        if (!len) return 1;
        char *arg = (char*)alloca(len);
        if (!WideCharToMultiByte(CP_UTF8, 0, warg, -1, arg, len, NULL, NULL)) return 1;
        argv[i] = (wchar_t*)arg;
    }
#endif
#ifdef JULIA_ENABLE_THREADING
    // We need to make sure this function is called before any reference to
    // TLS variables. Since the compiler is free to move calls to
    // `jl_get_ptls_states()` around, we should avoid referencing TLS
    // variables in this function. (Mark `true_main` as noinline for this
    // reason).
    jl_set_ptls_states_getter(jl_get_ptls_states_static);
#endif
    libsupport_init();
    parse_opts(&argc, (char***)&argv);
    if (lisp_prompt) {
        jl_lisp_prompt();
        return 0;
    }
    julia_init(imagepathspecified ? JL_IMAGE_CWD : JL_IMAGE_JULIA_HOME);
    int ret = true_main(argc, (char**)argv);
    jl_atexit_hook(ret);
    return ret;
}

#ifdef __cplusplus
}
#endif

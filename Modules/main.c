/* Python interpreter main program */

#include "Python.h"
#include "pycore_call.h"          // _PyObject_CallNoArgs()
#include "pycore_initconfig.h"    // _PyArgv
#include "pycore_interp.h"        // _PyInterpreterState.sysdict
#include "pycore_pathconfig.h"    // _PyPathConfig_ComputeSysPath0()
#include "pycore_pylifecycle.h"   // _Py_PreInitializeFromPyArgv()
#include "pycore_pystate.h"       // _PyInterpreterState_GET()

/* Includes for exit_sigint() */
#include <stdio.h>                // perror()
#ifdef HAVE_SIGNAL_H
#  include <signal.h>             // SIGINT
#endif
#if defined(HAVE_GETPID) && defined(HAVE_UNISTD_H)
#  include <unistd.h>             // getpid()
#endif
#ifdef MS_WINDOWS
#  include <windows.h>            // STATUS_CONTROL_C_EXIT
#endif
/* End of includes for exit_sigint() */

#define COPYRIGHT \
    "Type \"help\", \"copyright\", \"credits\" or \"license\" " \
    "for more information."

#ifdef __cplusplus
extern "C" {
#endif

/* --- pymain_init() ---------------------------------------------- */

static PyStatus
pymain_init(const _PyArgv *args)
{   // @ pymain_main
    // 加载程序执行参数、构造系统运行配置，完成 Python 系统框架初始化

    PyStatus status;

    // 初始化（创建）动态运行时实例（单例）
    // 相当于 Python 自身的程序实体，也就是类似于 `app` 对象的概念
    status = _PyRuntime_Initialize();
    if (_PyStatus_EXCEPTION(status)) {
        return status;
    }

    // 创建并初始化 PyPreConfig 为默认配置信息
    PyPreConfig preconfig;
    PyPreConfig_InitPythonConfig(&preconfig);

    // 从 args（包括系统环境变量）中加载和解析 PyPreConfig 配置信息，并将其设置到 PyRuntime 的 preconfig 属性
    status = _Py_PreInitializeFromPyArgv(&preconfig, args);
    if (_PyStatus_EXCEPTION(status)) {
        return status;
    }

    // 创建并初始化 PyConfig 为默认配置信息
    PyConfig config;
    PyConfig_InitPythonConfig(&config);

    // 将命令行输入的参数 args 赋值到 config 的 `argv` 属性
    // 注意：这里并没有解析和构造任何配置信息
    /* pass NULL as the config: config is read from command line arguments,
       environment variables, configuration files */
    if (args->use_bytes_argv) {
        status = PyConfig_SetBytesArgv(&config, args->argc, args->bytes_argv);
    }
    else {
        status = PyConfig_SetArgv(&config, args->argc, args->wchar_argv);
    }
    if (_PyStatus_EXCEPTION(status)) {
        goto done;
    }

    // 用 PyConfig 配置选项和参数，来初始化 Python 系统环境
    status = Py_InitializeFromConfig(&config);
    if (_PyStatus_EXCEPTION(status)) {
        goto done;
    }
    status = _PyStatus_OK();

done:
    PyConfig_Clear(&config);
    return status;
}


/* --- pymain_run_python() ---------------------------------------- */

/* Non-zero if filename, command (-c) or module (-m) is set
   on the command line */
static inline int config_run_code(const PyConfig *config)
{
    return (config->run_command != NULL
            || config->run_filename != NULL
            || config->run_module != NULL);
}


/* Return non-zero if stdin is a TTY or if -i command line option is used */
static int
stdin_is_interactive(const PyConfig *config)
{
    return (isatty(fileno(stdin)) || config->interactive);
}


/* Display the current Python exception and return an exitcode */
static int
pymain_err_print(int *exitcode_p)
{
    int exitcode;
    if (_Py_HandleSystemExit(&exitcode)) {
        *exitcode_p = exitcode;
        return 1;
    }

    PyErr_Print();
    return 0;
}


static int
pymain_exit_err_print(void)
{
    int exitcode = 1;
    pymain_err_print(&exitcode);
    return exitcode;
}


/* Write an exitcode into *exitcode and return 1 if we have to exit Python.
   Return 0 otherwise. */
static int
pymain_get_importer(const wchar_t *filename, PyObject **importer_p, int *exitcode)
{   // @pymain_run_python

    PyObject *sys_path0 = NULL, *importer;

    sys_path0 = PyUnicode_FromWideChar(filename, wcslen(filename));
    if (sys_path0 == NULL) {
        goto error;
    }

    importer = PyImport_GetImporter(sys_path0);
    if (importer == NULL) {
        goto error;
    }

    if (importer == Py_None) {
        Py_DECREF(sys_path0);
        Py_DECREF(importer);
        return 0;
    }

    Py_DECREF(importer);
    *importer_p = sys_path0;
    return 0;

error:
    Py_XDECREF(sys_path0);

    PySys_WriteStderr("Failed checking if argv[0] is an import path entry\n");
    return pymain_err_print(exitcode);
}


static int
pymain_sys_path_add_path0(PyInterpreterState *interp, PyObject *path0)
{   // @ pymain_run_python

    _Py_IDENTIFIER(path);
    PyObject *sys_path;
    PyObject *sysdict = interp->sysdict;
    if (sysdict != NULL) {
        sys_path = _PyDict_GetItemIdWithError(sysdict, &PyId_path);
        if (sys_path == NULL && PyErr_Occurred()) {
            return -1;
        }
    }
    else {
        sys_path = NULL;
    }
    if (sys_path == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "unable to get sys.path");
        return -1;
    }

    if (PyList_Insert(sys_path, 0, path0)) {
        return -1;
    }
    return 0;
}


static void
pymain_header(const PyConfig *config)
{   // @ pymain_run_python
    //（运行 Python 程序前，向 stdin）打印 Python 头信息：版本、版权

    if (config->quiet) {
        return;
    }

    if (!config->verbose && (config_run_code(config) || !stdin_is_interactive(config))) {
        return;
    }

    fprintf(stderr, "Python %s on %s\n", Py_GetVersion(), Py_GetPlatform());
    if (config->site_import) {
        fprintf(stderr, "%s\n", COPYRIGHT);
    }
}


static void
pymain_import_readline(const PyConfig *config)
{   // @ pymain_run_python
    //（根据需要）默认导入 `readline` 模块

    if (config->isolated) {
        return;
    }
    if (!config->inspect && config_run_code(config)) {
        return;
    }
    if (!isatty(fileno(stdin))) {
        return;
    }

    PyObject *mod = PyImport_ImportModule("readline");
    if (mod == NULL) {
        PyErr_Clear();
    }
    else {
        Py_DECREF(mod);
    }
}


static int
pymain_run_command(wchar_t *command)
{   // @ pymain_run_python

    PyObject *unicode, *bytes;
    int ret;

    unicode = PyUnicode_FromWideChar(command, -1);
    if (unicode == NULL) {
        goto error;
    }

    if (PySys_Audit("cpython.run_command", "O", unicode) < 0) {
        return pymain_exit_err_print();
    }

    bytes = PyUnicode_AsUTF8String(unicode);
    Py_DECREF(unicode);
    if (bytes == NULL) {
        goto error;
    }

    PyCompilerFlags cf = _PyCompilerFlags_INIT;
    cf.cf_flags |= PyCF_IGNORE_COOKIE;
    ret = PyRun_SimpleStringFlags(PyBytes_AsString(bytes), &cf);
    Py_DECREF(bytes);
    return (ret != 0);

error:
    PySys_WriteStderr("Unable to decode the command from the command line:\n");
    return pymain_exit_err_print();
}


static int
pymain_run_module(const wchar_t *modname, int set_argv0)
{   // @ pymain_run_python

    PyObject *module, *runpy, *runmodule, *runargs, *result;
    if (PySys_Audit("cpython.run_module", "u", modname) < 0) {
        return pymain_exit_err_print();
    }
    runpy = PyImport_ImportModule("runpy");
    if (runpy == NULL) {
        fprintf(stderr, "Could not import runpy module\n");
        return pymain_exit_err_print();
    }
    runmodule = PyObject_GetAttrString(runpy, "_run_module_as_main");
    if (runmodule == NULL) {
        fprintf(stderr, "Could not access runpy._run_module_as_main\n");
        Py_DECREF(runpy);
        return pymain_exit_err_print();
    }
    module = PyUnicode_FromWideChar(modname, wcslen(modname));
    if (module == NULL) {
        fprintf(stderr, "Could not convert module name to unicode\n");
        Py_DECREF(runpy);
        Py_DECREF(runmodule);
        return pymain_exit_err_print();
    }
    runargs = PyTuple_Pack(2, module, set_argv0 ? Py_True : Py_False);
    if (runargs == NULL) {
        fprintf(stderr,
            "Could not create arguments for runpy._run_module_as_main\n");
        Py_DECREF(runpy);
        Py_DECREF(runmodule);
        Py_DECREF(module);
        return pymain_exit_err_print();
    }
    _Py_UnhandledKeyboardInterrupt = 0;
    result = PyObject_Call(runmodule, runargs, NULL);
    if (!result && PyErr_Occurred() == PyExc_KeyboardInterrupt) {
        _Py_UnhandledKeyboardInterrupt = 1;
    }
    Py_DECREF(runpy);
    Py_DECREF(runmodule);
    Py_DECREF(module);
    Py_DECREF(runargs);
    if (result == NULL) {
        return pymain_exit_err_print();
    }
    Py_DECREF(result);
    return 0;
}


static int
pymain_run_file_obj(PyObject *program_name, PyObject *filename,
                    int skip_source_first_line)
{   // @ pymain_run_file

    if (PySys_Audit("cpython.run_file", "O", filename) < 0) {
        return pymain_exit_err_print();
    }

    FILE *fp = _Py_fopen_obj(filename, "rb");
    if (fp == NULL) {
        // Ignore the OSError
        PyErr_Clear();
        PySys_FormatStderr("%S: can't open file %R: [Errno %d] %s\n",
                           program_name, filename, errno, strerror(errno));
        return 2;
    }

    if (skip_source_first_line) {
        int ch;
        /* Push back first newline so line numbers remain the same */
        while ((ch = getc(fp)) != EOF) {
            if (ch == '\n') {
                (void)ungetc(ch, fp);
                break;
            }
        }
    }

    struct _Py_stat_struct sb;
    if (_Py_fstat_noraise(fileno(fp), &sb) == 0 && S_ISDIR(sb.st_mode)) {
        PySys_FormatStderr("%S: %R is a directory, cannot continue\n",
                           program_name, filename);
        fclose(fp);
        return 1;
    }

    // Call pending calls like signal handlers (SIGINT)
    if (Py_MakePendingCalls() == -1) {
        fclose(fp);
        return pymain_exit_err_print();
    }

    /* PyRun_AnyFileExFlags(closeit=1) calls fclose(fp) before running code */
    PyCompilerFlags cf = _PyCompilerFlags_INIT;
    int run = _PyRun_AnyFileObject(fp, filename, 1, &cf);
    return (run != 0);
}

static int
pymain_run_file(const PyConfig *config)
{   // @ pymain_run_python

    PyObject *filename = PyUnicode_FromWideChar(config->run_filename, -1);
    if (filename == NULL) {
        PyErr_Print();
        return -1;
    }
    PyObject *program_name = PyUnicode_FromWideChar(config->program_name, -1);
    if (program_name == NULL) {
        Py_DECREF(filename);
        PyErr_Print();
        return -1;
    }

    int res = pymain_run_file_obj(program_name, filename,
                                  config->skip_source_first_line);
    Py_DECREF(filename);
    Py_DECREF(program_name);
    return res;
}


static int
pymain_run_startup(PyConfig *config, int *exitcode)
{   // @ pymain_run_stdin

    int ret;
    if (!config->use_environment) {
        return 0;
    }
    PyObject *startup = NULL;
#ifdef MS_WINDOWS
    const wchar_t *env = _wgetenv(L"PYTHONSTARTUP");
    if (env == NULL || env[0] == L'\0') {
        return 0;
    }
    startup = PyUnicode_FromWideChar(env, wcslen(env));
    if (startup == NULL) {
        goto error;
    }
#else
    const char *env = _Py_GetEnv(config->use_environment, "PYTHONSTARTUP");
    if (env == NULL) {
        return 0;
    }
    startup = PyUnicode_DecodeFSDefault(env);
    if (startup == NULL) {
        goto error;
    }
#endif
    if (PySys_Audit("cpython.run_startup", "O", startup) < 0) {
        goto error;
    }

    FILE *fp = _Py_fopen_obj(startup, "r");
    if (fp == NULL) {
        int save_errno = errno;
        PyErr_Clear();
        PySys_WriteStderr("Could not open PYTHONSTARTUP\n");

        errno = save_errno;
        PyErr_SetFromErrnoWithFilenameObjects(PyExc_OSError, startup, NULL);
        goto error;
    }

    PyCompilerFlags cf = _PyCompilerFlags_INIT;
    (void) _PyRun_SimpleFileObject(fp, startup, 0, &cf);
    PyErr_Clear();
    fclose(fp);
    ret = 0;

done:
    Py_XDECREF(startup);
    return ret;

error:
    ret = pymain_err_print(exitcode);
    goto done;
}


/* Write an exitcode into *exitcode and return 1 if we have to exit Python.
   Return 0 otherwise. */
static int
pymain_run_interactive_hook(int *exitcode)
{   // @ pymain_run_stdin
    // @ pymain_repl

    PyObject *sys, *hook, *result;
    sys = PyImport_ImportModule("sys");
    if (sys == NULL) {
        goto error;
    }

    hook = PyObject_GetAttrString(sys, "__interactivehook__");
    Py_DECREF(sys);
    if (hook == NULL) {
        PyErr_Clear();
        return 0;
    }

    if (PySys_Audit("cpython.run_interactivehook", "O", hook) < 0) {
        goto error;
    }

    result = _PyObject_CallNoArgs(hook);
    Py_DECREF(hook);
    if (result == NULL) {
        goto error;
    }
    Py_DECREF(result);

    return 0;

error:
    PySys_WriteStderr("Failed calling sys.__interactivehook__\n");
    return pymain_err_print(exitcode);
}


static int
pymain_run_stdin(PyConfig *config)
{   // @ pymain_run_python

    if (stdin_is_interactive(config)) {
        config->inspect = 0;
        Py_InspectFlag = 0; /* do exit on SystemExit */

        int exitcode;
        if (pymain_run_startup(config, &exitcode)) {
            return exitcode;
        }

        if (pymain_run_interactive_hook(&exitcode)) {
            return exitcode;
        }
    }

    /* call pending calls like signal handlers (SIGINT) */
    if (Py_MakePendingCalls() == -1) {
        return pymain_exit_err_print();
    }

    if (PySys_Audit("cpython.run_stdin", NULL) < 0) {
        return pymain_exit_err_print();
    }

    PyCompilerFlags cf = _PyCompilerFlags_INIT;
    int run = PyRun_AnyFileExFlags(stdin, "<stdin>", 0, &cf);
    return (run != 0);
}


static void
pymain_repl(PyConfig *config, int *exitcode)
{   // @ pymain_run_python

    /* Check this environment variable at the end, to give programs the
       opportunity to set it from Python. */
    // 最后一次获取系统环境变量 $PYTHONINSPECT 的值。也就是允许 Python 程序的运行的过程中，设置该环境变量
    if (!config->inspect && _Py_GetEnv(config->use_environment, "PYTHONINSPECT")) {
        config->inspect = 1;
        Py_InspectFlag = 1;
    }

    // 如果不需要执行 pymain_run_interactive_hook 处理，则直接返回
    if (!(config->inspect && stdin_is_interactive(config) && config_run_code(config))) {
        return;
    }

    config->inspect = 0;
    Py_InspectFlag = 0;
    if (pymain_run_interactive_hook(exitcode)) {
        return;
    }

    PyCompilerFlags cf = _PyCompilerFlags_INIT;
    int res = PyRun_AnyFileFlags(stdin, "<stdin>", &cf);
    *exitcode = (res != 0);
}


static void
pymain_run_python(int *exitcode)
{   // @ Py_RunMain
    // 运行 python 程序，并返回运行退出码

    // 获取解释器
    PyInterpreterState *interp = _PyInterpreterState_GET();

    /* pymain_run_stdin() modify the config */

    // 获取配置信息（包括要运行的 Python 程序信息）
    PyConfig *config = (PyConfig*)_PyInterpreterState_GetConfig(interp);

    // 判断要运行的 Python 脚本（程序）是否存在 Importer
    PyObject *main_importer_path = NULL;
    if (config->run_filename != NULL) {

        /* If filename is a package (ex: directory or ZIP file) which contains
           __main__.py, main_importer_path is set to filename and will be
           prepended to sys.path.

           Otherwise, main_importer_path is left unchanged. */
        // 判断要执行的 Python 脚本（程序）是否存在 Importer
        // + 首先，这个函数返回到值，表示的是操作是否成功。如果成功返回 0；否则失败，程序退出。
        //   而关于是否存在 Importer，则是通过 main_importer_path 的返回值来体现。
        //   - 如果没有返回值。即保持入参值 NULL 不变，说明不存在 Importer
        //   - 如果存在 Importer，则 main_importer_path 会将 run_filename(字符串类型) 转换为 PyObject 类型返回。
        // + 关于是否存在 Importer
        //   如果存在 Importer，往往说明 Python 脚本（程序）是一个包（目录或 zip 文件），其下会存在 __main__.py
        if (pymain_get_importer(config->run_filename, &main_importer_path,
                                exitcode)) {
            return;
        }
    }

    // 要运行的 Python 脚本（程序）存在 Importer，则将脚本所在目录作为 `path0`，添加到导入路径中
    if (main_importer_path != NULL) {
        if (pymain_sys_path_add_path0(interp, main_importer_path) < 0) {
            goto error;
        }
    }
    else if (!config->isolated) {

        // 其他情况下，将 "当前运行路径" 作为 `path0`，添加到导入路径中

        PyObject *path0 = NULL;
        int res = _PyPathConfig_ComputeSysPath0(&config->argv, &path0);
        if (res < 0) {
            goto error;
        }

        if (res > 0) {
            if (pymain_sys_path_add_path0(interp, path0) < 0) {
                Py_DECREF(path0);
                goto error;
            }
            Py_DECREF(path0);
        }
    }

    // 打印 Python 头信息：版本、版权
    pymain_header(config);

    // （根据需要）默认导入 `readline` 模块
    pymain_import_readline(config);

    if (config->run_command) {
        *exitcode = pymain_run_command(config->run_command);
    }
    else if (config->run_module) {
        *exitcode = pymain_run_module(config->run_module, 1);
    }
    else if (main_importer_path != NULL) {
        *exitcode = pymain_run_module(L"__main__", 0);
    }
    else if (config->run_filename != NULL) {
        *exitcode = pymain_run_file(config);
    }
    else {
        *exitcode = pymain_run_stdin(config);
    }

    pymain_repl(config, exitcode);
    goto done;

error:
    *exitcode = pymain_exit_err_print();

done:
    Py_XDECREF(main_importer_path);
}


/* --- pymain_main() ---------------------------------------------- */

static void
pymain_free(void)
{   // @ Py_RunMain
    // @ pymain_main
    // @ pymain_exit_error

    _PyImport_Fini2();

    /* Free global variables which cannot be freed in Py_Finalize():
       configuration options set before Py_Initialize() which should
       remain valid after Py_Finalize(), since
       Py_Initialize()-Py_Finalize() can be called multiple times. */
    _PyPathConfig_ClearGlobal();
    _Py_ClearStandardStreamEncoding();
    _Py_ClearArgcArgv();
    _PyRuntime_Finalize();
}


static int
exit_sigint(void)
{   // @ Py_RunMain

    /* bpo-1054041: We need to exit via the
     * SIG_DFL handler for SIGINT if KeyboardInterrupt went unhandled.
     * If we don't, a calling process such as a shell may not know
     * about the user's ^C.  https://www.cons.org/cracauer/sigint.html */
#if defined(HAVE_GETPID) && !defined(MS_WINDOWS)
    if (PyOS_setsig(SIGINT, SIG_DFL) == SIG_ERR) {
        perror("signal");  /* Impossible in normal environments. */
    } else {
        kill(getpid(), SIGINT);
    }
    /* If setting SIG_DFL failed, or kill failed to terminate us,
     * there isn't much else we can do aside from an error code. */
#endif  /* HAVE_GETPID && !MS_WINDOWS */
#ifdef MS_WINDOWS
    /* cmd.exe detects this, prints ^C, and offers to terminate. */
    /* https://msdn.microsoft.com/en-us/library/cc704588.aspx */
    return STATUS_CONTROL_C_EXIT;
#else
    return SIGINT + 128;
#endif  /* !MS_WINDOWS */
}


static void _Py_NO_RETURN
pymain_exit_error(PyStatus status)
{   // @ pymain_main
    // Python 系统框架在初始化过程中，出现错误

    if (_PyStatus_IS_EXIT(status)) {
        /* If it's an error rather than a regular exit, leave Python runtime
           alive: Py_ExitStatusException() uses the current exception and use
           sys.stdout in this case. */
        pymain_free();
    }

    Py_ExitStatusException(status);
}


int
Py_RunMain(void)
{   // @ extern
    // @ pymain_main
    // 运行 python 主程序

    // 运行 python 程序，并返回运行退出码
    int exitcode = 0;
    pymain_run_python(&exitcode);

    // 完成 Python 系统框架的退出流程处理
    // 包括发信号、等待退出等操作
    if (Py_FinalizeEx() < 0) {
        /* Value unlikely to be confused with a non-error exit status or
           other special meaning */
        exitcode = 120;
    }

    // 执行系统资源析构处理
    pymain_free();

    if (_Py_UnhandledKeyboardInterrupt) {
        exitcode = exit_sigint();
    }

    return exitcode;
}


static int
pymain_main(_PyArgv *args)
{   // @ Py_Main
    // @ Py_BytesMain

    // 加载程序执行参数、构造系统运行配置，完成 Python 系统框架初始化
    PyStatus status = pymain_init(args);

    // 在初始化过程中，直接退出
    if (_PyStatus_IS_EXIT(status)) {
        pymain_free();
        return status.exitcode;
    }

    // 在初始化过程中，出现错误
    if (_PyStatus_EXCEPTION(status)) {
        pymain_exit_error(status);
    }

    // 运行 python 主程序
    return Py_RunMain();
}


int
Py_Main(int argc, wchar_t **argv)
{
    _PyArgv args = {
        .argc = argc,
        .use_bytes_argv = 0,
        .bytes_argv = NULL,
        .wchar_argv = argv};
    return pymain_main(&args);
}


int
Py_BytesMain(int argc, char **argv)
{
    _PyArgv args = {
        .argc = argc,
        .use_bytes_argv = 1,
        .bytes_argv = argv,
        .wchar_argv = NULL};
    return pymain_main(&args);
}

#ifdef __cplusplus
}
#endif

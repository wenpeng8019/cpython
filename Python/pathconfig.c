/* Path configuration like module_search_path (sys.path) */

#include "Python.h"
#include "osdefs.h"               // DELIM
#include "pycore_initconfig.h"
#include "pycore_fileutils.h"
#include "pycore_pathconfig.h"
#include "pycore_pymem.h"         // _PyMem_SetDefaultAllocator()
#include <wchar.h>
#ifdef MS_WINDOWS
#  include <windows.h>            // GetFullPathNameW(), MAX_PATH
#endif

#ifdef __cplusplus
extern "C" {
#endif

// - 定义: 路径设置管理对象（全局变量）
// + 该路径设置是给 SDK 接口来设定的。
//   其中设置的值最终会被合并到 config 中，而不是直接作为程序运行后的路径状态值
//   不过，在 interpreter_update_config 和 pyinit_core_reconfigure 时，
//   这里的值又会被程序最终的路径配置信息，通过 _PyConfig_WritePathConfig 写入
//   因此，这里的路径配置信息，和程序最终运行后的路径状态值，是同步的
_PyPathConfig _Py_path_config = _PyPathConfig_INIT;

static int
copy_wstr(wchar_t **dst, const wchar_t *src)
{
    assert(*dst == NULL);
    if (src != NULL) {
        *dst = _PyMem_RawWcsdup(src);
        if (*dst == NULL) {
            return -1;
        }
    }
    else {
        *dst = NULL;
    }
    return 0;
}


static void
pathconfig_clear(_PyPathConfig *config)
{
    /* _PyMem_SetDefaultAllocator() is needed to get a known memory allocator,
       since Py_SetPath(), Py_SetPythonHome() and Py_SetProgramName() can be
       called before Py_Initialize() which can changes the memory allocator. */
    PyMemAllocatorEx old_alloc;
    _PyMem_SetDefaultAllocator(PYMEM_DOMAIN_RAW, &old_alloc);

#define CLEAR(ATTR) \
    do { \
        PyMem_RawFree(ATTR); \
        ATTR = NULL; \
    } while (0)

    CLEAR(config->program_full_path);
    CLEAR(config->prefix);
    CLEAR(config->exec_prefix);
    CLEAR(config->stdlib_dir);
    CLEAR(config->module_search_path);
    CLEAR(config->program_name);
    CLEAR(config->home);
#ifdef MS_WINDOWS
    CLEAR(config->base_executable);
#endif

#undef CLEAR

    PyMem_SetAllocator(PYMEM_DOMAIN_RAW, &old_alloc);
}


static PyStatus
pathconfig_copy(_PyPathConfig *config, const _PyPathConfig *config2)
{
    pathconfig_clear(config);

#define COPY_ATTR(ATTR) \
    do { \
        if (copy_wstr(&config->ATTR, config2->ATTR) < 0) { \
            return _PyStatus_NO_MEMORY(); \
        } \
    } while (0)

    COPY_ATTR(program_full_path);
    COPY_ATTR(prefix);
    COPY_ATTR(exec_prefix);
    COPY_ATTR(module_search_path);
    COPY_ATTR(stdlib_dir);
    COPY_ATTR(program_name);
    COPY_ATTR(home);
#ifdef MS_WINDOWS
    config->isolated = config2->isolated;
    config->site_import = config2->site_import;
    COPY_ATTR(base_executable);
#endif

#undef COPY_ATTR

    return _PyStatus_OK();
}


// 清空（全局）路径管理对象（_Py_path_config）中的数据
void
_PyPathConfig_ClearGlobal(void)
{   // 清除 

    PyMemAllocatorEx old_alloc;
    _PyMem_SetDefaultAllocator(PYMEM_DOMAIN_RAW, &old_alloc);

    pathconfig_clear(&_Py_path_config);

    PyMem_SetAllocator(PYMEM_DOMAIN_RAW, &old_alloc);
}


static wchar_t*
_PyWideStringList_Join(const PyWideStringList *list, wchar_t sep)
{
    size_t len = 1;   /* NUL terminator */
    for (Py_ssize_t i=0; i < list->length; i++) {
        if (i != 0) {
            len++;
        }
        len += wcslen(list->items[i]);
    }

    wchar_t *text = PyMem_RawMalloc(len * sizeof(wchar_t));
    if (text == NULL) {
        return NULL;
    }
    wchar_t *str = text;
    for (Py_ssize_t i=0; i < list->length; i++) {
        wchar_t *path = list->items[i];
        if (i != 0) {
            *str++ = sep;
        }
        len = wcslen(path);
        memcpy(str, path, len * sizeof(wchar_t));
        str += len;
    }
    *str = L'\0';

    return text;
}


static PyStatus
pathconfig_set_from_config(_PyPathConfig *pathconfig, const PyConfig *config)
{   // @ _PyConfig_WritePathConfig
    // @ pathconfig_init
    // 将 PyConfig 中的和 Path 有关的设定，复制到 pathconfig（_PyPathConfig）
    
    PyStatus status;
    PyMemAllocatorEx old_alloc;
    _PyMem_SetDefaultAllocator(PYMEM_DOMAIN_RAW, &old_alloc);

    if (config->module_search_paths_set) {
        PyMem_RawFree(pathconfig->module_search_path);
        pathconfig->module_search_path = _PyWideStringList_Join(&config->module_search_paths, DELIM);
        if (pathconfig->module_search_path == NULL) {
            goto no_memory;
        }
    }

#define COPY_CONFIG(PATH_ATTR, CONFIG_ATTR) \
        if (config->CONFIG_ATTR) { \
            PyMem_RawFree(pathconfig->PATH_ATTR); \
            pathconfig->PATH_ATTR = NULL; \
            if (copy_wstr(&pathconfig->PATH_ATTR, config->CONFIG_ATTR) < 0) { \
                goto no_memory; \
            } \
        }

    COPY_CONFIG(program_full_path, executable);
    COPY_CONFIG(prefix, prefix);
    COPY_CONFIG(exec_prefix, exec_prefix);
    COPY_CONFIG(stdlib_dir, stdlib_dir);
    COPY_CONFIG(program_name, program_name);
    COPY_CONFIG(home, home);
#ifdef MS_WINDOWS
    COPY_CONFIG(base_executable, base_executable);
#endif

#undef COPY_CONFIG

    status = _PyStatus_OK();
    goto done;

no_memory:
    status = _PyStatus_NO_MEMORY();

done:
    PyMem_SetAllocator(PYMEM_DOMAIN_RAW, &old_alloc);
    return status;
}

// 将（全局）路径管理对象（_Py_path_config）中的数据以 dict 数据类型返回 
PyObject *
_PyPathConfig_AsDict(void)
{   
    PyObject *dict = PyDict_New();
    if (dict == NULL) {
        return NULL;
    }

#define SET_ITEM(KEY, EXPR) \
        do { \
            PyObject *obj = (EXPR); \
            if (obj == NULL) { \
                goto fail; \
            } \
            int res = PyDict_SetItemString(dict, KEY, obj); \
            Py_DECREF(obj); \
            if (res < 0) { \
                goto fail; \
            } \
        } while (0)
#define SET_ITEM_STR(KEY) \
        SET_ITEM(#KEY, \
            (_Py_path_config.KEY \
             ? PyUnicode_FromWideChar(_Py_path_config.KEY, -1) \
             : (Py_INCREF(Py_None), Py_None)))
#define SET_ITEM_INT(KEY) \
        SET_ITEM(#KEY, PyLong_FromLong(_Py_path_config.KEY))

    SET_ITEM_STR(program_full_path);
    SET_ITEM_STR(prefix);
    SET_ITEM_STR(exec_prefix);
    SET_ITEM_STR(module_search_path);
    SET_ITEM_STR(stdlib_dir);
    SET_ITEM_STR(program_name);
    SET_ITEM_STR(home);
#ifdef MS_WINDOWS
    SET_ITEM_INT(isolated);
    SET_ITEM_INT(site_import);
    SET_ITEM_STR(base_executable);

    {
        wchar_t py3path[MAX_PATH];
        HMODULE hPython3 = GetModuleHandleW(PY3_DLLNAME);
        PyObject *obj;
        if (hPython3
            && GetModuleFileNameW(hPython3, py3path, Py_ARRAY_LENGTH(py3path)))
        {
            obj = PyUnicode_FromWideChar(py3path, -1);
            if (obj == NULL) {
                goto fail;
            }
        }
        else {
            obj = Py_None;
            Py_INCREF(obj);
        }
        if (PyDict_SetItemString(dict, "python3_dll", obj) < 0) {
            Py_DECREF(obj);
            goto fail;
        }
        Py_DECREF(obj);
    }
#endif

#undef SET_ITEM
#undef SET_ITEM_STR
#undef SET_ITEM_INT

    return dict;

fail:
    Py_DECREF(dict);
    return NULL;
}

// 将 PyConfig 中的和 Path 有关的设定，写入到（全局）管理对象（_Py_path_config）中
PyStatus
_PyConfig_WritePathConfig(const PyConfig *config)
{   // @ extern
    // @ pyinit_core_reconfigure
    // @ interpreter_update_config

    return pathconfig_set_from_config(&_Py_path_config, config);
}


static PyStatus
config_init_module_search_paths(PyConfig *config, _PyPathConfig *pathconfig)
{   // @ config_init_pathconfig

    assert(!config->module_search_paths_set);

    _PyWideStringList_Clear(&config->module_search_paths);

    const wchar_t *sys_path = pathconfig->module_search_path;
    const wchar_t delim = DELIM;
    while (1) {
        const wchar_t *p = wcschr(sys_path, delim);
        if (p == NULL) {
            p = sys_path + wcslen(sys_path); /* End of string */
        }

        size_t path_len = (p - sys_path);
        wchar_t *path = PyMem_RawMalloc((path_len + 1) * sizeof(wchar_t));
        if (path == NULL) {
            return _PyStatus_NO_MEMORY();
        }
        memcpy(path, sys_path, path_len * sizeof(wchar_t));
        path[path_len] = L'\0';

        PyStatus status = PyWideStringList_Append(&config->module_search_paths, path);
        PyMem_RawFree(path);
        if (_PyStatus_EXCEPTION(status)) {
            return status;
        }

        if (*p == '\0') {
            break;
        }
        sys_path = p + 1;
    }
    config->module_search_paths_set = 1;
    return _PyStatus_OK();
}


/* Calculate the path configuration:

   - exec_prefix
   - module_search_path
   - stdlib_dir
   - prefix
   - program_full_path

   On Windows, more fields are calculated:

   - base_executable
   - isolated
   - site_import

   On other platforms, isolated and site_import are left unchanged, and
   _PyConfig_InitPathConfig() copies executable to base_executable (if it's not
   set).

   Priority, highest to lowest:

   - PyConfig
   - _Py_path_config: set by Py_SetPath(), Py_SetPythonHome()
     and Py_SetProgramName()
   - _PyPathConfig_Calculate()
*/
static PyStatus
pathconfig_init(_PyPathConfig *pathconfig, const PyConfig *config,
                int compute_path_config)
{   // @ config_init_pathconfig

    PyStatus status;

    PyMemAllocatorEx old_alloc;
    _PyMem_SetDefaultAllocator(PYMEM_DOMAIN_RAW, &old_alloc);

    // （优先）读取（由 SDK 接口设定的）全局（路径设置）变量中设定的值，作为默认值
    status = pathconfig_copy(pathconfig, &_Py_path_config);
    if (_PyStatus_EXCEPTION(status)) {
        goto done;
    }

    // 如果 PyConfig 直接设定了和 Path 相关配置，则用 PyConfig 设定的值重载默认值
    status = pathconfig_set_from_config(pathconfig, config);
    if (_PyStatus_EXCEPTION(status)) {
        goto done;
    }

    // 对于 pathconfig 所有值为 NULL 的选项（即还未被赋值的选项），自动解析获取它们的值
    if (compute_path_config) {
        status = _PyPathConfig_Calculate(pathconfig, config);
    }

done:
    PyMem_SetAllocator(PYMEM_DOMAIN_RAW, &old_alloc);
    return status;
}


static PyStatus
config_init_pathconfig(PyConfig *config, int compute_path_config)
{   // @ _PyConfig_InitPathConfig
    // 初始化 PyConfig 中和 Path 相关的设定

    // 创建一个局部的 _PyPathConfig 对象
    _PyPathConfig pathconfig = _PyPathConfig_INIT;
    PyStatus status;

    // 初始化（加载、解析）路径配置（到 pathconfig 对象）
    status = pathconfig_init(&pathconfig, config, compute_path_config);
    if (_PyStatus_EXCEPTION(status)) {
        goto done;
    }

    // 用（计算得到的）pathconfig 的 module_search_path 来初始化构造 config 的 module_search_paths
    if (!config->module_search_paths_set
        && pathconfig.module_search_path != NULL)
    {
        status = config_init_module_search_paths(config, &pathconfig);
        if (_PyStatus_EXCEPTION(status)) {
            goto done;
        }
    }

    // 将（计算得到的）pathconfig 中的（其他）路径信息，复制到 config 中

#define COPY_ATTR(PATH_ATTR, CONFIG_ATTR) \
        if (config->CONFIG_ATTR == NULL && pathconfig.PATH_ATTR != NULL) { \
            if (copy_wstr(&config->CONFIG_ATTR, pathconfig.PATH_ATTR) < 0) { \
                goto no_memory; \
            } \
        }

#ifdef MS_WINDOWS
    if (config->executable != NULL && config->base_executable == NULL) {
        /* If executable is set explicitly in the configuration,
           ignore calculated base_executable: _PyConfig_InitPathConfig()
           will copy executable to base_executable */
    }
    else {
        COPY_ATTR(base_executable, base_executable);
    }
#endif

    COPY_ATTR(program_full_path, executable);
    COPY_ATTR(prefix, prefix);
    COPY_ATTR(exec_prefix, exec_prefix);
    COPY_ATTR(stdlib_dir, stdlib_dir);

#undef COPY_ATTR

#ifdef MS_WINDOWS
    /* If a ._pth file is found: isolated and site_import are overridden */
    if (pathconfig.isolated != -1) {
        config->isolated = pathconfig.isolated;
    }
    if (pathconfig.site_import != -1) {
        config->site_import = pathconfig.site_import;
    }
#endif

    status = _PyStatus_OK();
    goto done;

no_memory:
    status = _PyStatus_NO_MEMORY();

done:
    pathconfig_clear(&pathconfig);
    return status;
}


// 初始化 PyConfig 中的，和 Path 有关的设定
PyStatus
_PyConfig_InitPathConfig(PyConfig *config, int compute_path_config)
{   // @ extern
    // @ config_init_import


    /* Do we need to calculate the path? */
    if (!config->module_search_paths_set
        || config->executable == NULL
        || config->prefix == NULL
        || config->exec_prefix == NULL)
    {
        PyStatus status = config_init_pathconfig(config, compute_path_config);
        if (_PyStatus_EXCEPTION(status)) {
            return status;
        }
    }

    // 将 prefix 备份复制到 base_prefix，因为 prefix 可以被虚拟环境重载
    if (config->base_prefix == NULL && config->prefix != NULL) {
        if (copy_wstr(&config->base_prefix, config->prefix) < 0) {
            return _PyStatus_NO_MEMORY();
        }
    }

    // 将 exec_prefix 备份复制到 base_exec_prefix，因为 exec_prefix 可以被虚拟环境重载
    if (config->base_exec_prefix == NULL && config->exec_prefix != NULL) {
        if (copy_wstr(&config->base_exec_prefix,
                      config->exec_prefix) < 0) {
            return _PyStatus_NO_MEMORY();
        }
    }

    // 将 executable 备份复制到 base_executable，因为 executable 可以被虚拟环境重载
    // 这里 config->executable 对应的就是 pathconfig->program_full_path
    if (config->base_executable == NULL && config->executable != NULL) {
        if (copy_wstr(&config->base_executable,
                      config->executable) < 0) {
            return _PyStatus_NO_MEMORY();
        }
    }

    return _PyStatus_OK();
}


/* External interface */

static void _Py_NO_RETURN
path_out_of_memory(const char *func)
{
    _Py_FatalErrorFunc(func, "out of memory");
}

// 设置（全局）管理对象（_Py_path_config）中的 `module_search_path` 属性
void
Py_SetPath(const wchar_t *path)
{
    if (path == NULL) {
        pathconfig_clear(&_Py_path_config);
        return;
    }

    PyMemAllocatorEx old_alloc;
    _PyMem_SetDefaultAllocator(PYMEM_DOMAIN_RAW, &old_alloc);

    PyMem_RawFree(_Py_path_config.prefix);
    PyMem_RawFree(_Py_path_config.exec_prefix);
    PyMem_RawFree(_Py_path_config.stdlib_dir);
    PyMem_RawFree(_Py_path_config.module_search_path);

    _Py_path_config.prefix = _PyMem_RawWcsdup(L"");
    _Py_path_config.exec_prefix = _PyMem_RawWcsdup(L"");
    // XXX Copy this from the new module_search_path?
    if (_Py_path_config.home != NULL) {
        _Py_path_config.stdlib_dir = _PyMem_RawWcsdup(_Py_path_config.home);
    }
    else {
        _Py_path_config.stdlib_dir = _PyMem_RawWcsdup(L"");
    }
    _Py_path_config.module_search_path = _PyMem_RawWcsdup(path);

    PyMem_SetAllocator(PYMEM_DOMAIN_RAW, &old_alloc);

    if (_Py_path_config.prefix == NULL
        || _Py_path_config.exec_prefix == NULL
        || _Py_path_config.stdlib_dir == NULL
        || _Py_path_config.module_search_path == NULL)
    {
        path_out_of_memory(__func__);
    }
}


// 设置（全局）管理对象（_Py_path_config）中的 `home` 属性
void
Py_SetPythonHome(const wchar_t *home)
{
    if (home == NULL) {
        return;
    }

    PyMemAllocatorEx old_alloc;
    _PyMem_SetDefaultAllocator(PYMEM_DOMAIN_RAW, &old_alloc);

    PyMem_RawFree(_Py_path_config.home);
    _Py_path_config.home = _PyMem_RawWcsdup(home);

    PyMem_SetAllocator(PYMEM_DOMAIN_RAW, &old_alloc);

    if (_Py_path_config.home == NULL) {
        path_out_of_memory(__func__);
    }
}


// 设置（全局）管理对象（_Py_path_config）中的 `program_name` 属性
void
Py_SetProgramName(const wchar_t *program_name)
{
    if (program_name == NULL || program_name[0] == L'\0') {
        return;
    }

    PyMemAllocatorEx old_alloc;
    _PyMem_SetDefaultAllocator(PYMEM_DOMAIN_RAW, &old_alloc);

    PyMem_RawFree(_Py_path_config.program_name);
    _Py_path_config.program_name = _PyMem_RawWcsdup(program_name);

    PyMem_SetAllocator(PYMEM_DOMAIN_RAW, &old_alloc);

    if (_Py_path_config.program_name == NULL) {
        path_out_of_memory(__func__);
    }
}

// 设置（全局）管理对象（_Py_path_config）中的 `program_full_path` 属性
void
_Py_SetProgramFullPath(const wchar_t *program_full_path)
{
    if (program_full_path == NULL || program_full_path[0] == L'\0') {
        return;
    }

    PyMemAllocatorEx old_alloc;
    _PyMem_SetDefaultAllocator(PYMEM_DOMAIN_RAW, &old_alloc);

    PyMem_RawFree(_Py_path_config.program_full_path);
    _Py_path_config.program_full_path = _PyMem_RawWcsdup(program_full_path);

    PyMem_SetAllocator(PYMEM_DOMAIN_RAW, &old_alloc);

    if (_Py_path_config.program_full_path == NULL) {
        path_out_of_memory(__func__);
    }
}


wchar_t *
Py_GetPath(void)
{
    return _Py_path_config.module_search_path;
}


wchar_t *
_Py_GetStdlibDir(void)
{
    wchar_t *stdlib_dir = _Py_path_config.stdlib_dir;
    if (stdlib_dir != NULL && stdlib_dir[0] != L'\0') {
        return stdlib_dir;
    }
    return NULL;
}


wchar_t *
Py_GetPrefix(void)
{
    return _Py_path_config.prefix;
}


wchar_t *
Py_GetExecPrefix(void)
{
    return _Py_path_config.exec_prefix;
}


wchar_t *
Py_GetProgramFullPath(void)
{
    return _Py_path_config.program_full_path;
}


wchar_t*
Py_GetPythonHome(void)
{
    return _Py_path_config.home;
}


wchar_t *
Py_GetProgramName(void)
{
    return _Py_path_config.program_name;
}

/* Compute module search path from argv[0] or the current working
   directory ("-m module" case) which will be prepended to sys.argv:
   sys.path[0].

   Return 1 if the path is correctly resolved and written into *path0_p.

   Return 0 if it fails to resolve the full path. For example, return 0 if the
   current working directory has been removed (bpo-36236) or if argv is empty.

   Raise an exception and return -1 on error.
   */
int
_PyPathConfig_ComputeSysPath0(const PyWideStringList *argv, PyObject **path0_p)
{
    assert(_PyWideStringList_CheckConsistency(argv));

    if (argv->length == 0) {
        /* Leave sys.path unchanged if sys.argv is empty */
        return 0;
    }

    wchar_t *argv0 = argv->items[0];
    int have_module_arg = (wcscmp(argv0, L"-m") == 0);
    int have_script_arg = (!have_module_arg && (wcscmp(argv0, L"-c") != 0));

    wchar_t *path0 = argv0;
    Py_ssize_t n = 0;

#ifdef HAVE_REALPATH
    wchar_t fullpath[MAXPATHLEN];
#elif defined(MS_WINDOWS)
    wchar_t fullpath[MAX_PATH];
#endif

    if (have_module_arg) {
#if defined(HAVE_REALPATH) || defined(MS_WINDOWS)
        if (!_Py_wgetcwd(fullpath, Py_ARRAY_LENGTH(fullpath))) {
            return 0;
        }
        path0 = fullpath;
#else
        path0 = L".";
#endif
        n = wcslen(path0);
    }

#ifdef HAVE_READLINK
    wchar_t link[MAXPATHLEN + 1];
    int nr = 0;
    wchar_t path0copy[2 * MAXPATHLEN + 1];

    if (have_script_arg) {
        nr = _Py_wreadlink(path0, link, Py_ARRAY_LENGTH(link));
    }
    if (nr > 0) {
        /* It's a symlink */
        link[nr] = '\0';
        if (link[0] == SEP) {
            path0 = link; /* Link to absolute path */
        }
        else if (wcschr(link, SEP) == NULL) {
            /* Link without path */
        }
        else {
            /* Must join(dirname(path0), link) */
            wchar_t *q = wcsrchr(path0, SEP);
            if (q == NULL) {
                /* path0 without path */
                path0 = link;
            }
            else {
                /* Must make a copy, path0copy has room for 2 * MAXPATHLEN */
                wcsncpy(path0copy, path0, MAXPATHLEN);
                q = wcsrchr(path0copy, SEP);
                wcsncpy(q+1, link, MAXPATHLEN);
                q[MAXPATHLEN + 1] = L'\0';
                path0 = path0copy;
            }
        }
    }
#endif /* HAVE_READLINK */

    wchar_t *p = NULL;

#if SEP == '\\'
    /* Special case for Microsoft filename syntax */
    if (have_script_arg) {
        wchar_t *q;
#if defined(MS_WINDOWS)
        /* Replace the first element in argv with the full path. */
        wchar_t *ptemp;
        if (GetFullPathNameW(path0,
                           Py_ARRAY_LENGTH(fullpath),
                           fullpath,
                           &ptemp)) {
            path0 = fullpath;
        }
#endif
        p = wcsrchr(path0, SEP);
        /* Test for alternate separator */
        q = wcsrchr(p ? p : path0, '/');
        if (q != NULL)
            p = q;
        if (p != NULL) {
            n = p + 1 - path0;
            if (n > 1 && p[-1] != ':')
                n--; /* Drop trailing separator */
        }
    }
#else
    /* All other filename syntaxes */
    if (have_script_arg) {
#if defined(HAVE_REALPATH)
        if (_Py_wrealpath(path0, fullpath, Py_ARRAY_LENGTH(fullpath))) {
            path0 = fullpath;
        }
#endif
        p = wcsrchr(path0, SEP);
    }
    if (p != NULL) {
        n = p + 1 - path0;
#if SEP == '/' /* Special case for Unix filename syntax */
        if (n > 1) {
            /* Drop trailing separator */
            n--;
        }
#endif /* Unix */
    }
#endif /* All others */

    PyObject *path0_obj = PyUnicode_FromWideChar(path0, n);
    if (path0_obj == NULL) {
        return -1;
    }

    *path0_p = path0_obj;
    return 1;
}


#ifdef MS_WINDOWS
#define WCSTOK wcstok_s
#else
#define WCSTOK wcstok
#endif

/* Search for a prefix value in an environment file (pyvenv.cfg).

   - If found, copy it into *value_p: string which must be freed by
     PyMem_RawFree().
   - If not found, *value_p is set to NULL.
*/
PyStatus
_Py_FindEnvConfigValue(FILE *env_file, const wchar_t *key,
                       wchar_t **value_p)
{
    *value_p = NULL;

    char buffer[MAXPATHLEN * 2 + 1];  /* allow extra for key, '=', etc. */
    buffer[Py_ARRAY_LENGTH(buffer)-1] = '\0';

    while (!feof(env_file)) {
        char * p = fgets(buffer, Py_ARRAY_LENGTH(buffer) - 1, env_file);

        if (p == NULL) {
            break;
        }

        size_t n = strlen(p);
        if (p[n - 1] != '\n') {
            /* line has overflowed - bail */
            break;
        }
        if (p[0] == '#') {
            /* Comment - skip */
            continue;
        }

        wchar_t *tmpbuffer = _Py_DecodeUTF8_surrogateescape(buffer, n, NULL);
        if (tmpbuffer) {
            wchar_t * state;
            wchar_t * tok = WCSTOK(tmpbuffer, L" \t\r\n", &state);
            if ((tok != NULL) && !wcscmp(tok, key)) {
                tok = WCSTOK(NULL, L" \t", &state);
                if ((tok != NULL) && !wcscmp(tok, L"=")) {
                    tok = WCSTOK(NULL, L"\r\n", &state);
                    if (tok != NULL) {
                        *value_p = _PyMem_RawWcsdup(tok);
                        PyMem_RawFree(tmpbuffer);

                        if (*value_p == NULL) {
                            return _PyStatus_NO_MEMORY();
                        }

                        /* found */
                        return _PyStatus_OK();
                    }
                }
            }
            PyMem_RawFree(tmpbuffer);
        }
    }

    /* not found */
    return _PyStatus_OK();
}

#ifdef __cplusplus
}
#endif

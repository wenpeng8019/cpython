#ifndef Py_INTERNAL_PATHCONFIG_H
#define Py_INTERNAL_PATHCONFIG_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

typedef struct _PyPathConfig {

    /* Full path to the Python program */
    wchar_t *program_full_path;
    wchar_t *prefix;
    wchar_t *exec_prefix;
    wchar_t *stdlib_dir;
    /* Set by Py_SetPath(), or computed by _PyConfig_InitPathConfig() */
    wchar_t *module_search_path;

    /* Python program name */
    // Python 程序名，用于反向解析计算 program_full_path
    // 最终目的是解析其所在路径，从而确定 stdlib 位置（可能的位置之一）
    wchar_t *program_name;
    // 用于重载 stdlib 所在目录
    // 使得默认相对于 program_full_path 的 stdlib 目录，可被重置到任何目标位置
    /* Set by Py_SetPythonHome() or PYTHONHOME environment variable */
    wchar_t *home;

#ifdef MS_WINDOWS
    /* isolated and site_import are used to set Py_IsolatedFlag and
       Py_NoSiteFlag flags on Windows in read_pth_file(). These fields
       are ignored when their value are equal to -1 (unset). */
    int isolated;
    int site_import;
    /* Set when a venv is detected */
    wchar_t *base_executable;
#endif
} _PyPathConfig;

#ifdef MS_WINDOWS
#  define _PyPathConfig_INIT \
      {.module_search_path = NULL, \
       .isolated = -1, \
       .site_import = -1}
#else
#  define _PyPathConfig_INIT \
      {.module_search_path = NULL}
#endif
/* Note: _PyPathConfig_INIT sets other fields to 0/NULL */

PyAPI_DATA(_PyPathConfig) _Py_path_config;
#ifdef MS_WINDOWS
PyAPI_DATA(wchar_t*) _Py_dll_path;
#endif

extern void _PyPathConfig_ClearGlobal(void);

extern PyStatus _PyPathConfig_Calculate(
    _PyPathConfig *pathconfig,
    const PyConfig *config);
extern int _PyPathConfig_ComputeSysPath0(
    const PyWideStringList *argv,
    PyObject **path0);
extern PyStatus _Py_FindEnvConfigValue(
    FILE *env_file,
    const wchar_t *key,
    wchar_t **value_p);

#ifdef MS_WINDOWS
extern wchar_t* _Py_GetDLLPath(void);
#endif

extern PyStatus _PyConfig_WritePathConfig(const PyConfig *config);
extern void _Py_DumpPathConfig(PyThreadState *tstate);
extern PyObject* _PyPathConfig_AsDict(void);

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_PATHCONFIG_H */

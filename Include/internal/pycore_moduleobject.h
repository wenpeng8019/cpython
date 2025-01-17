#ifndef Py_INTERNAL_MODULEOBJECT_H
#define Py_INTERNAL_MODULEOBJECT_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

// 模块对象（基）类
typedef struct {

    // 对象管理链表指针
    PyObject_HEAD

    // 模块域下的数据对象集合
    PyObject *md_dict;

    struct PyModuleDef *md_def;

    void *md_state;

    // 引用了该模块的对象的指针
    PyObject *md_weaklist;

    // 模块命名
    // for logging purposes after md_dict is cleared
    PyObject *md_name;

} PyModuleObject;

static inline PyModuleDef* _PyModule_GetDef(PyObject *mod) {
    assert(PyModule_Check(mod));
    return ((PyModuleObject *)mod)->md_def;
}

static inline void* _PyModule_GetState(PyObject* mod) {
    assert(PyModule_Check(mod));
    return ((PyModuleObject *)mod)->md_state;
}

static inline PyObject* _PyModule_GetDict(PyObject *mod) {
    assert(PyModule_Check(mod));
    PyObject *dict = ((PyModuleObject *)mod) -> md_dict;
    // _PyModule_GetDict(mod) must not be used after calling module_clear(mod)
    assert(dict != NULL);
    return dict;
}

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_MODULEOBJECT_H */

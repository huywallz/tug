#ifndef TUGLIB_H
#define TUGLIB_H

#include <math.h>
#include <limits.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include "tug.h"

#define tuglib_isnew(T) (tug_getstate(T) == TUG_NEW)
#define tuglib_iserr(T) (tug_getstate(T) == TUG_ERROR)
#define tuglib_isalive(T) (tug_getstate(T) == TUG_ALIVE)
#define tuglib_ispaused(T) (tug_getstate(T) == TUG_PAUSED)
#define tuglib_isdead(T) (tug_getstate(T) == TUG_DEAD)
#define tuglib_isyield(T) (tuglib_isnew(T) || tuglib_ispaused(T))

static const char* tuglib_typename(tug_Type type) {
    switch (type) {
        case TUG_STR: return "str";
        case TUG_NUM: return "num";
	    case TUG_TRUE:
	    case TUG_FALSE: return "bool";
        case TUG_NIL: return "nil";
        case TUG_FUNC: return "func";
        case TUG_TABLE: return "table";
        case TUG_TUPLE:
        case TUG_UNKNOWN:
        default: return "unknown";
    }
}

static tug_Object* tuglib_checkany(tug_Task* T, size_t idx) {
    if (!tug_hasarg(T, idx)) {
        tug_err(T, "missing argument #%zu", idx + 1);
    }

    return tug_getarg(T, idx);
}

static tug_Object* tuglib_checktype(tug_Task* T, size_t idx, tug_Type expected) {
    tug_Object* obj = tuglib_checkany(T, idx);
    tug_Type type = tug_gettype(obj);
    if (type != expected) {
        tug_err(T, "argument #%zu expected '%s', got '%s'", idx + 1, tuglib_typename(expected), tuglib_typename(type));
    }

    return obj;
}

static const char* tuglib_checkstr(tug_Task* T, size_t idx) {
    tug_Object* obj = tuglib_checktype(T, idx, TUG_STR);
    if (tuglib_iserr(T)) return NULL;

    return tug_getstr(obj);
}

static double tuglib_checknum(tug_Task* T, size_t idx) {
    tug_Object* obj = tuglib_checktype(T, idx, TUG_NUM);
    if (tuglib_iserr(T)) return 0;

    return tug_getnum(obj);
}

static int tuglib_checkint(tug_Task* T, size_t idx) {
    double num = tuglib_checknum(T, idx);
    if (tuglib_iserr(T)) return 0;

    if (num < (double)INT_MIN || num > (double)INT_MAX || floor(num) != num) {
        tug_err(T, "argument #%zu expected '<int>', got '<double>'", idx + 1);
    }
    
    return (int)num;
}

static long tuglib_checklong(tug_Task* T, size_t idx) {
    double num = tuglib_checknum(T, idx);
    if (tuglib_iserr(T)) return 0;

    if (fabs(num) > 9007199254740992.0 || floor(num) != num) {
        tug_err(T, "argument #%zu expected '<long>', got '<double>'", idx + 1);
    }
    
    return (long)num;
}

static int tuglib_checkbool(tug_Task* T, size_t idx) {
    tug_Object* obj = tuglib_checkany(T, idx);
    tug_Type type = tug_gettype(obj);
    if (type == TUG_TRUE) return 1;
    else if (type == TUG_FALSE) return 0;
    
    tug_err(T, "argument #%zu expected 'bool', got '%s'", idx + 1, tuglib_typename(type));
}

#define tuglib_checknil(T, idx) (tuglib_checktype(T, idx, TUG_FUNC))
#define tuglib_checkfunc(T, idx) (tuglib_checktype(T, idx, TUG_FUNC))
#define tuglib_checktable(T, idx) (tuglib_checktype(T, idx, TUG_TABLE))

#define tuglib_isany(T, idx) tug_hasarg((T), (idx))
#define tuglib_isnone(T, idx) (!tuglib_isany(T, idx))
#define tuglib_istype(T, idx, type) (tuglib_isnone(T, idx) ? -1 : tug_gettype(tug_getarg(T, idx)) == (type))
#define tuglib_isstr(T, idx) tuglib_istype(T, idx, TUG_STR)

static int tuglib_isbool(tug_Task* T, size_t idx) {
    if (tuglib_isnone(T, idx)) return -1;
    tug_Object* obj = tug_getarg(T, idx);
    tug_Type type = tug_gettype(obj);

    return type == TUG_TRUE || type == TUG_FALSE;
}

#define tuglib_isnil(T, idx) tuglib_istype(T, idx, TUG_NIL)
#define tuglib_isfunc(T, idx) tuglib_istype(T, idx, TUG_FUNC)
#define tuglib_istable(T, idx) tuglib_istype(T, idx, TUG_TABLE)

#define tuglib_optany(T, idx, def) (tuglib_isnone((T), (idx)) ? (def) : tuglib_checkany((T), (idx)))
#define tuglib_opttype(T, idx, expected, def) (tuglib_isnone((T), (idx)) ? (def) : tuglib_checktype((T), (idx), (expected)))
#define tuglib_optnum(T, idx, def) (tuglib_isnone((T), (idx)) ? (def) : tuglib_checknum((T), (idx)))
#define tuglib_optint(T, idx, def) (tuglib_isnone((T), (idx)) ? (def) : tuglib_checkint((T), (idx)))
#define tuglib_optlong(T, idx, def) (tuglib_isnone((T), (idx)) ? (def) : tuglib_checklong((T), (idx)))
#define tuglib_optstr(T, idx, def) (tuglib_isnone((T), (idx)) ? (def) : tuglib_checkstr((T), (idx)))
#define tuglib_optbool(T, idx, def) (tuglib_isnone((T), (idx)) ? (def) : tuglib_checkbool((T), (idx)))
#define tuglib_optnil(T, idx, def) (tuglib_isnone((T), (idx)) ? (def) : tuglib_checknil((T), (idx)))
#define tuglib_optfunc(T, idx, def) (tuglib_isnone((T), (idx)) ? (def) : tuglib_checkfunc((T), (idx)))
#define tuglib_opttable(T, idx, def) (tuglib_isnone((T), (idx)) ? (def) : tuglib_checktable((T), (idx)))

char* tuglib_tostr(tug_Object* obj) {
    switch (tug_gettype(obj)) {
        case TUG_STR: return strdup(tug_getstr(obj));
        case TUG_NUM: {
            char* res = malloc(50);
            snprintf(res, 50, "%.17g", tug_getnum(obj));

            return res;
        }
        case TUG_TRUE: return strdup("true");
        case TUG_FALSE: return strdup("false");
        case TUG_NIL: return strdup("nil");
        case TUG_FUNC: {
            char* res = malloc(50);
			snprintf(res, 50, "func: 0x%lx", tug_getid(obj));

            return res;
        }
        case TUG_TABLE: {
            char* res = malloc(50);
			snprintf(res, 50, "table: 0x%lx", tug_getid(obj));

            return res;
        }
        default: return strdup("<unknown>");
    }
}

void __tuglib_print(tug_Task* T) {
    size_t argc = tug_getargc(T);
    for (size_t i = 0; i < argc; i++) {
        tug_Object* obj = tug_getarg(T, i);

        char* str = tuglib_tostr(obj);
        printf("%s%s", str, i + 1 == argc ? "" : "\t");
        free(str);
    }

    printf("\n");
}

void __tuglib_tostr(tug_Task* T) {
    tug_Object* obj = tuglib_checkany(T, 0);
    
    char* str = tuglib_tostr(obj);
    tug_ret(T, tug_str((const char*)str));
    free(str);
}

void __tuglib_type(tug_Task* T) {
    tug_Object* obj = tuglib_checkany(T, 0);
    
    tug_ret(T, tug_str(tuglib_typename(tug_gettype(obj))));
}

void __tuglib_setmetatable(tug_Task* T) {
    tug_Object* table = tuglib_checktable(T, 0);
    tug_Object* mtable = tuglib_checktable(T, 1);

    tug_setmetatable(table, mtable);
    tug_ret(T, table);
}

void __tuglib_getmetatable(tug_Task* T) {
    tug_Object* table = tuglib_checktable(T, 0);
    tug_Object* mtable = tug_getmetatable(table);

    if (mtable == tug_nil) return;
    tug_Object* obj = tug_getindex(mtable, tug_str("__metatable"));
    if (obj == tug_nil) tug_ret(T, mtable);
    else tug_ret(T, obj);
}

void __tuglib_test(tug_Task* T) {
    tug_Object* obj = tuglib_checkfunc(T, 0);

    tug_calls(T, obj, 0);
}

void tuglib_loadbuiltins(tug_Task* T) {
    tug_setglobal(T, "print", tug_cfunc("print", __tuglib_print));
    tug_setglobal(T, "tostr", tug_cfunc("tostr", __tuglib_tostr));
    tug_setglobal(T, "type", tug_cfunc("type", __tuglib_type));
    tug_setglobal(T, "setmetatable", tug_cfunc("setmetatable", __tuglib_setmetatable));
    tug_setglobal(T, "getmetatable", tug_cfunc("getmetatable", __tuglib_getmetatable));
    
    tug_setglobal(T, "test", tug_cfunc("test", __tuglib_test));
}

void tuglib_loadlibs(tug_Task* T) {
    tuglib_loadbuiltins(T);
}

#endif
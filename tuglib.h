#ifndef TUGLIB_H
#define TUGLIB_H

#include <stddef.h>
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

static tug_Object* tuglib_checkarg(tug_Task* T, size_t idx) {
    if (!tug_hasarg(T, idx)) {
        tug_err(T, "bad argument #%lu (missing value)", idx + 1);
        return tug_nil;
    }

    return tug_getarg(T, idx);
}

static tug_Object* tuglib_checktype(tug_Task* T, size_t idx, tug_Type expected) {
    tug_Object* obj = tuglib_checkarg(T, idx);
    tug_Type type = tug_gettype(obj);
    if (type != expected) {
        tug_err(T, "bad argument #%lu (%s expected, got %s)", idx + 1, tuglib_typename(expected), tuglib_typename(type));
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

#define tuglib_checkint(T, idx) (int)tuglib_checknum((T), (idx));

static unsigned char tuglib_checkbool(tug_Task* T, size_t idx) {
    tug_Object* obj = tuglib_checkarg(T, idx);
    tug_Type type = tug_gettype(obj);
    if (type == TUG_TRUE) return 1;
    else if (type == TUG_FALSE) return 0;
    
    tug_err(T, "bad argument #%lu (%s expected, got %s)", idx + 1, "bool", tuglib_typename(type));
    return -1;
}

static unsigned char tuglib_checknil(tug_Task* T, size_t idx) {
    tuglib_checktype(T, idx, TUG_NIL);
    return !tuglib_iserr(T);
}

#define tuglib_isnone(T, idx) (!tug_hasarg((T), (idx)))

#define tuglib_checkfunc(T, idx) (tuglib_checktype(T, idx, TUG_FUNC))

#endif
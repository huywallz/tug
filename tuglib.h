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

static const char* tuglib_typename(tug_ObjectType type) {
    switch (type) {
        case TUG_STR: return "str";
        case TUG_NUM: return "num";
	    case TUG_TRUE:
	    case TUG_FALSE: return "bool";
        case TUG_NIL: return "nil";
        case TUG_FUNC:
        case TUG_CFUNC: return "func";
        case TUG_TABLE: return "table";
        case TUG_TUPLE:
        case TUG_UNKNOWN:
        default: return "unknown";
    }
}

static tug_Object* tuglib_checkarg(tug_Task* T, size_t idx, tug_ObjectType expected) {
    if (!tug_hasarg(T, idx)) {
        tug_err(T, "bad argument #%lu (missing value)", idx + 1);
        return tug_nil;
    }

    tug_Object* obj = tug_getarg(T, idx);
    tug_ObjectType type = tug_gettype(obj);
    if (type != expected) {
        tug_err(T, "bad argument #%lu (%s expected, got %s)", idx + 1, tuglib_typename(expected), tuglib_typename(type));
    }

    return obj;
}

#endif
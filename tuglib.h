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

static tug_Object* tuglib_checkarg(tug_Task* T, size_t idx, tug_ObjectType expected) {
    if (!tuglib_isyield(T)) {
        tug_err()
    }
}

#endif
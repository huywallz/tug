#ifndef TUG_H
#define TUG_H

typedef struct tug_Task tug_Task;
typedef struct tug_Object tug_Object;

typedef enum {
        TUG_STR,
        TUG_NUM,
        TUG_TRUE,
        TUG_FALSE,
        TUG_NIL,
        TUG_FUNC,
        TUG_TABLE,
        TUG_TUPLE,
        TUG_LIST,
        TUG_UNKNOWN,
} tug_Type;

typedef enum {
        TUG_NEW,
        TUG_PAUSED,
        TUG_ALIVE,
        TUG_ERROR,
        TUG_DEAD,
} tug_TaskState;

void tug_init(void);
void tug_close(void);

extern tug_Object* tug_true;
extern tug_Object* tug_false;
extern tug_Object* tug_nil;

tug_Object* tug_str(char* str);
tug_Object* tug_conststr(const char* str);
tug_Object* tug_num(double num);
tug_Object* tug_table(void);

typedef void(*tug_CFunc)(tug_Task*);
tug_Object* tug_cfunc(const char* name, tug_CFunc func);

unsigned long tug_getid(tug_Object* obj);
tug_Type tug_gettype(tug_Object* obj);

const char* tug_getstr(tug_Object* obj);
double tug_getnum(tug_Object* obj);
void tug_setfield(tug_Object* obj, tug_Object* key, tug_Object* value);
tug_Object* tug_getfield(tug_Object* obj, tug_Object* key);
size_t tug_getlen(tug_Object* obj);
void tug_setmetatable(tug_Object* obj, tug_Object* metatable);
tug_Object* tug_getmetatable(tug_Object* obj);

tug_Object* tug_tuple(void);
void tug_tuplepush(tug_Object* tuple, tug_Object* obj);
tug_Object* tug_tuplepop(tug_Object* tuple);

void tug_setvar(tug_Task* T, const char* name, tug_Object* value);
tug_Object* tug_getvar(tug_Task* T, const char* name);
int tug_hasvar(tug_Task* T, const char* name);

void tug_setglobal(tug_Task* T, const char* name, tug_Object* value);
tug_Object* tug_getglobal(tug_Task* T, const char* name);
int tug_hasglobal(tug_Task* T, const char* name);

size_t tug_getargc(tug_Task* T);
tug_Object* tug_getarg(tug_Task* T, size_t idx);
int tug_hasarg(tug_Task* T, size_t idx);

tug_Object* tug_calls(tug_Task* T, tug_Object* func, size_t n, ...);
tug_Object* tug_pcalls(tug_Task* T, int* errptr, tug_Object* func, size_t n, ...);
tug_Object* tug_call(tug_Task* T, tug_Object* func, tug_Object* arg);
tug_Object* tug_pcall(tug_Task* T, int* errptr, tug_Object* func, tug_Object* arg);
void tug_rets(tug_Task* T, size_t n, ...);
void tug_ret(tug_Task* T, tug_Object* obj);

void tug_err(tug_Task* T, const char* fmt, ...);
const char* tug_getmsg(tug_Task* T);
const char* tug_geterr(tug_Task* T);

tug_Task* tug_task(const char* src, const char* code, char* errmsg);
void tug_resume(tug_Task* T);
void tug_pause(tug_Task* T);
tug_TaskState tug_getstate(tug_Task* T);

#endif
#ifndef TUGLIB_H
#define TUGLIB_H

#include <math.h>
#include <limits.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <ctype.h>
#include "tug.h"

#define tuglib_isnew(T) (tug_getstate(T) == TUG_NEW)
#define tuglib_iserr(T) (tug_getstate(T) == TUG_ERROR)
#define tuglib_isalive(T) (tug_getstate(T) == TUG_ALIVE)
#define tuglib_ispaused(T) (tug_getstate(T) == TUG_PAUSED)
#define tuglib_isdead(T) (tug_getstate(T) == TUG_DEAD)
#define tuglib_isyield(T) (tuglib_isnew(T) || tuglib_ispaused(T))

static int tuglib_hasmetatable(tug_Object* obj) {
	if (tug_gettype(obj) == TUG_TABLE) return tug_getmetatable(obj) != tug_nil;
	return 0;
}

static tug_Object* tuglib_getmetafield(tug_Object* obj, const char* key) {
	if (!tuglib_hasmetatable(obj)) return tug_nil;
	tug_Object* mtable = tug_getmetatable(obj);
	return tug_getfield(mtable, tug_conststr(key));
}

static int tuglib_setmetafield(tug_Object* obj, const char* key, tug_Object* value) {
	if (!tuglib_hasmetatable(obj)) return 0;
	tug_Object* mtable = tug_getmetatable(obj);
	tug_setfield(mtable, tug_conststr(key), value);
	
	return 1;
}

static const char* tuglib_typename(tug_Type type) {
	switch (type) {
		case TUG_STR: return "str";
		case TUG_NUM: return "num";
			case TUG_TRUE:
			case TUG_FALSE: return "bool";
		case TUG_NIL: return "nil";
		case TUG_FUNC: return "func";
		case TUG_TABLE: return "table";
		case TUG_LIST: return "list";
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
	return tug_getstr(obj);
}

static double tuglib_checknum(tug_Task* T, size_t idx) {
	tug_Object* obj = tuglib_checktype(T, idx, TUG_NUM);
	return tug_getnum(obj);
}

static int tuglib_checkint(tug_Task* T, size_t idx) {
	double num = tuglib_checknum(T, idx);
	if (num < (double)INT_MIN || num > (double)INT_MAX || floor(num) != num) {
		tug_err(T, "argument #%zu expected '<int>', got '<double>'", idx + 1);
	}

	return (int)num;
}

static long tuglib_checklong(tug_Task* T, size_t idx) {
	double num = tuglib_checknum(T, idx);
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
	return -1;
}

#define tuglib_checknil(T, idx) (tuglib_checktype(T, idx, TUG_FUNC))
#define tuglib_checkfunc(T, idx) (tuglib_checktype(T, idx, TUG_FUNC))
#define tuglib_checktable(T, idx) (tuglib_checktype(T, idx, TUG_TABLE))
#define tuglib_checklist(T, idx) (tuglib_checktype(T, idx, TUG_LIST))

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
#define tuglib_islist(T, idx) tuglib_istype(T, idx, TUG_LIST)

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
#define tuglib_optlist(T, idx, def) (tuglib_isnone((T), (idx)) ? (def) : tuglib_checklist((T), (idx)))

#define tuglib_gettypename(obj) tuglib_typename(tug_gettype((obj)))

static tug_Object* tuglib_tostr(tug_Object* obj) {
	tug_Type obj_type = tug_gettype(obj);
	switch (obj_type) {
		case TUG_STR: return obj;
		case TUG_NUM: {
			char* res = malloc(50);
			snprintf(res, 50, "%.17g", tug_getnum(obj));
			tug_Object* str_obj = tug_str(res);
			return str_obj;
		}
		case TUG_TRUE: return tug_conststr("true");
		case TUG_FALSE: return tug_conststr("false");
		case TUG_NIL: return tug_conststr("nil");
		case TUG_FUNC:
		case TUG_TABLE:
		case TUG_LIST: {
			char* res = malloc(50);
			snprintf(res, 50, "%s: 0x%lx", tuglib_typename(obj_type), tug_getid(obj));
			tug_Object* str_obj = tug_str(res);
			return str_obj;
		}
		default: return tug_conststr("<unknown>");
	}
}

static void __tuglib_print(tug_Task* T) {
	size_t argc = tug_getargc(T);
	for (size_t i = 0; i < argc; i++) {
		tug_Object* obj = tug_getarg(T, i);

		tug_Object* str_obj = tuglib_tostr(obj);
		printf("%s%s", tug_getstr(str_obj), i + 1 == argc ? "" : "\t");
	}

	printf("\n");
}

static void __tuglib_tostr(tug_Task* T) {
	tug_Object* obj = tuglib_checkany(T, 0);
	tug_Object* tostr = tuglib_getmetafield(obj, "__tostr");
	if (tostr != tug_nil) {
		tug_Object* res = tug_call(T, tostr, obj);
		tug_Type res_type = tug_gettype(res);
		if (res_type != TUG_STR) {
			tug_err(T, "metamethod '__tostr' must return 'str', got '%s'", tuglib_gettypename(res));
			return;
		}
		tug_ret(T, res);
		return;
	}

	tug_Object* str_obj = tuglib_tostr(obj);
	tug_ret(T, str_obj);
}

static void __tuglib_type(tug_Task* T) {
	tug_Object* obj = tuglib_checkany(T, 0);

	tug_Object* newtype = tuglib_getmetafield(obj, "__type");
	if (newtype != tug_nil) {
		if (tug_gettype(newtype) != TUG_STR) {
			tug_err(T, "metamethod '__type' must be 'str', got '%s'", tuglib_gettypename(newtype));
			return;
		}
	}
	else tug_ret(T, tug_conststr(tuglib_typename(tug_gettype(obj))));
}

static void __tuglib_len(tug_Task* T) {
	tug_Object* obj = tuglib_checkany(T, 0);
	tug_Object* func = tuglib_getmetafield(obj, "__len");
	if (func != tug_nil) {
		tug_Object* res = tug_call(T, func, obj);
		tug_Type res_type = tug_gettype(res);
		if (res_type != TUG_NUM) {
			tug_err(T, "metamethod '__len' must return 'num', got '%s'", tuglib_gettypename(res));
			return;
		}
		tug_ret(T, res);
	} else switch (tug_gettype(obj)) {
		case TUG_STR:
		case TUG_TABLE:
		case TUG_LIST: {
			tug_ret(T, tug_num((double)tug_getlen(obj)));
		} break;
		default: tug_err(T, "argument #1 expected 'table' or 'str', got '%s'", tuglib_gettypename(obj));
	}
}

static void __tuglib_setmetatable(tug_Task* T) {
	tug_Object* table = tuglib_checktable(T, 0);
	tug_Object* mtable = tuglib_checktable(T, 1);

	tug_setmetatable(table, mtable);
	tug_ret(T, table);
}

static void __tuglib_getmetatable(tug_Task* T) {
	tug_Object* table = tuglib_checktable(T, 0);
	tug_Object* mtable = tug_getmetatable(table);

	if (mtable == tug_nil) return;
	tug_Object* obj = tug_getfield(mtable, tug_conststr("__metatable"));
	if (obj == tug_nil) tug_ret(T, mtable);
	else tug_ret(T, obj);
}

static void __tuglib_error(tug_Task* T) {
	tug_Object* obj = tuglib_checkany(T, 0);
	tug_Object* str_obj = tuglib_tostr(obj);
	tug_err(T, "%s", tug_getstr(str_obj));
}

static void __tuglib_pcall(tug_Task* T) {
	tug_Object* func = tuglib_checkfunc(T, 0);
	tug_Object* args = tug_tuple();
	for (size_t i = 1; i < tug_getargc(T); i++) {
		tug_tuplepush(args, tug_getarg(T, i));
	}

	int err = 0;
	tug_Object* res = tug_pcall(T, &err, func, args);
	if (err) {
		tug_rets(T, 2, tug_false, tug_conststr(tug_getmsg(T)));
	} else {
		tug_rets(T, 2, tug_true, res);
	}
}

static void __tuglib_tonum(tug_Task* T) {
	tug_Object* obj = tuglib_checkany(T, 0);
	tug_Type type = tug_gettype(obj);
	if (type == TUG_NUM) {
		tug_ret(T, obj);
		return;
	} else if (type == TUG_STR) {
		const char* str = tug_getstr(obj);
		char* endptr;
		double val = strtod(str, &endptr);
		if (endptr != str && *endptr == '\0') {
			tug_ret(T, tug_num(val));
			return;
		}
	}

	tug_ret(T, tug_nil);
}

static void __tuglib_assert(tug_Task* T) {
	tug_Object* obj = tuglib_checkany(T, 0);

	int is_true = 0;
	tug_Type type = tug_gettype(obj);
	switch (type) {
		case TUG_STR: is_true = strlen(tug_getstr(obj)) > 0; break;
		case TUG_NUM: is_true = tug_getnum(obj) != 0.0; break;
		case TUG_FUNC:
		case TUG_TRUE: is_true = 1; break;
		case TUG_LIST: is_true = tug_getlen(obj) != 0;
		case TUG_TABLE: {
			tug_Object* truth_obj = tuglib_getmetafield(obj, "__truth");
			if (truth_obj != tug_nil) {
				tug_Object* ret = tug_call(T, truth_obj, obj);
				tug_Type ret_type = tug_gettype(ret);
				if (ret_type == TUG_TRUE) is_true = 1;
				else if (ret_type == TUG_FALSE || ret_type == TUG_NIL) is_true = 0;
				else {
					tug_err(T, "metamethod '__truth' must return 'bool', got '%s'", tuglib_gettypename(ret));
					return;
				}
			} else is_true = tug_getlen(obj) != 0;
		} break;
		default: is_true = 0;
	}

	if (!is_true) {
		tug_Object* errobj = tug_nil;
		if (tuglib_isany(T, 1)) {
			errobj = tuglib_checkany(T, 1);
		}
		tug_Object* str_obj = tuglib_tostr(errobj);
		tug_err(T, "%s", tug_getstr(str_obj));
	}
}

static void __tuglib_rawget(tug_Task* T) {
	tug_Object* table = tuglib_checktable(T, 0);
	tug_Object* key = tuglib_checkany(T, 1);

	tug_Object* value = tug_getfield(table, key);
	tug_ret(T, value);
}

static void __tuglib_rawset(tug_Task* T) {
	tug_Object* table = tuglib_checktable(T, 0);
	tug_Object* key = tuglib_checkany(T, 1);
	tug_Object* value = tuglib_checkany(T, 2);

	tug_setfield(table, key, value);
}

static void __tuglib_clock(tug_Task* T) {
	tug_ret(T, tug_num((double)clock() / CLOCKS_PER_SEC));
}

static void __tuglib_min(tug_Task* T) {
	double lowest = 0.0;
	int first = 1;
	size_t argc = tug_getargc(T);
	for (size_t i = 0; i < argc; i++) {
		double num = tuglib_checknum(T, i);
		if (first) {
			lowest = num;
			first = 0;
		} else if (num < lowest) {
			lowest = num;
		}
	}

	tug_ret(T, tug_num(lowest));
}

static void __tuglib_max(tug_Task* T) {
	double highest = 0.0;
	int first = 1;
	size_t argc = tug_getargc(T);
	for (size_t i = 0; i < argc; i++) {
		double num = tuglib_checknum(T, i);
		if (first) {
			highest = num;
			first = 0;
		} else if (num > highest) {
			highest = num;
		}
	}

	tug_ret(T, tug_num(highest));
}

#define __tuglib_pi (double)(3.1415926535897932384626433832795028841971693993751058209749445923078164062)
#define __tuglib_e (double)(2.718281828459045235360287471352662497757247093699959574966967)
#define __tuglib_tau (double)(6.2831853071795864769252867665590057683943387987502116419498891846156328124)
#define __tuglib_inf (HUGE_VAL)
#define __tuglib_nan (NAN)

static void __tuglib_deg(tug_Task* T) {
	tug_ret(T, tug_num(tuglib_checknum(T, 0) * 180.0 / __tuglib_pi));
}

static void __tuglib_rad(tug_Task* T) {
	tug_ret(T, tug_num(tuglib_checknum(T, 0) * __tuglib_pi / 180.0));
}

static void __tuglib_log(tug_Task* T) {
	tug_ret(T, tug_num(log(tuglib_checknum(T, 0))));
}

static void __tuglib_log10(tug_Task* T) {
	tug_ret(T, tug_num(log10(tuglib_checknum(T, 0))));
}

static void __tuglib_cbrt(tug_Task* T) {
	tug_ret(T, tug_num(cbrt(tuglib_checknum(T, 0))));
}

static void __tuglib_cosh(tug_Task* T) {
	tug_ret(T, tug_num(cosh(tuglib_checknum(T, 0))));
}

static void __tuglib_atanh(tug_Task* T) {
	tug_ret(T, tug_num(atanh(tuglib_checknum(T, 0))));
}

static void __tuglib_asinh(tug_Task* T) {
	tug_ret(T, tug_num(asinh(tuglib_checknum(T, 0))));
}

static void __tuglib_acosh(tug_Task* T) {
	tug_ret(T, tug_num(acosh(tuglib_checknum(T, 0))));
}

static void __tuglib_frexp(tug_Task* T) {
	int exp;
	double val = frexp(tuglib_checknum(T, 0), &exp);
	tug_rets(T, 2, tug_num(val), tug_num((double)exp));
}

static void __tuglib_trunc(tug_Task* T) {
	tug_ret(T, tug_num(trunc(tuglib_checknum(T, 0))));
}

static void __tuglib_ldexp(tug_Task* T) {
	double val = tuglib_checknum(T, 0);
	int exp = tuglib_checkint(T, 1);
	tug_ret(T, tug_num(ldexp(val, exp)));
}

static void __tuglib_tanh(tug_Task* T) {
	tug_ret(T, tug_num(tanh(tuglib_checknum(T, 0))));
}

static void __tuglib_sinh(tug_Task* T) {
	tug_ret(T, tug_num(sinh(tuglib_checknum(T, 0))));
}

static void __tuglib_exp(tug_Task* T) {
	tug_ret(T, tug_num(exp(tuglib_checknum(T, 0))));
}

static void __tuglib_sin(tug_Task* T) {
	tug_ret(T, tug_num(sin(tuglib_checknum(T, 0))));
}

static void __tuglib_cos(tug_Task* T) {
	tug_ret(T, tug_num(cos(tuglib_checknum(T, 0))));
}

static void __tuglib_tan(tug_Task* T) {
	tug_ret(T, tug_num(tan(tuglib_checknum(T, 0))));
}

static void __tuglib_atan2(tug_Task* T) {
	tug_ret(T, tug_num(atan2(tuglib_checknum(T, 0), tuglib_checknum(T, 1))));
}

static void __tuglib_asin(tug_Task* T) {
	tug_ret(T, tug_num(asin(tuglib_checknum(T, 0))));
}

static void __tuglib_acos(tug_Task* T) {
	tug_ret(T, tug_num(acos(tuglib_checknum(T, 0))));
}

static void __tuglib_sqrt(tug_Task* T) {
	tug_ret(T, tug_num(sqrt(tuglib_checknum(T, 0))));
}

static void __tuglib_pow(tug_Task* T) {
	tug_ret(T, tug_num(pow(tuglib_checknum(T, 0), tuglib_checknum(T, 1))));
}

static void __tuglib_hypot(tug_Task* T) {
	tug_ret(T, tug_num(hypot(tuglib_checknum(T, 0), tuglib_checknum(T, 1))));
}

static void __tuglib_floor(tug_Task* T) {
	tug_ret(T, tug_num(floor(tuglib_checknum(T, 0))));
}

static void __tuglib_ceil(tug_Task* T) {
	tug_ret(T, tug_num(ceil(tuglib_checknum(T, 0))));
}

static void __tuglib_round(tug_Task* T) {
	tug_ret(T, tug_num(round(tuglib_checknum(T, 0))));
}

static void __tuglib_mod(tug_Task* T) {
	tug_ret(T, tug_num(fmod(tuglib_checknum(T, 0), tuglib_checknum(T, 1))));
}

static void __tuglib_abs(tug_Task* T) {
	tug_ret(T, tug_num(fabs(tuglib_checknum(T, 0))));
}

static void __tuglib_seed(tug_Task* T) {
	srand(tuglib_checkint(T, 0));
}

static void __tuglib_rand(tug_Task* T) {
	size_t argc = tug_getargc(T);
	if (argc == 0) {
		tug_ret(T, tug_num((double)rand() / (double)RAND_MAX));
	} else if (argc == 1) {
		int m = tuglib_checkint(T, 0);

		tug_ret(T, tug_num(rand() % m));
	} else {
		int m = tuglib_checkint(T, 0);
		int n = tuglib_checkint(T, 1);

		tug_ret(T, tug_num(m + rand() % (n - m + 1)));
	}
}

static void __tuglib_sub(tug_Task* T) {
	const char* str = tuglib_checkstr(T, 0);
	long start = tuglib_optlong(T, 1, 0);
	start = start < 0 ? 0 : start;
	size_t len = strlen(str);
	long end = tuglib_optlong(T, 2, len);
	end = end < 0 ? 0 : end;
	end = (size_t)end > len ? len : end;
	if (start > end) {
		tug_ret(T, tug_conststr(""));
		return;
	}
	
	size_t flen = (size_t)(end - start);
	char* res = malloc(flen + 1);
	memcpy(res, str + start, flen);
	res[flen] = '\0';
	tug_ret(T, tug_str(res));
}

static void __tuglib_concat(tug_Task* T) {
	size_t argc = tug_getargc(T);
	const char** strs = malloc(sizeof(const char*) * argc);
	size_t len = 0;
	for (size_t i = 0; i < argc; i++) {
		const char* str = tuglib_checkstr(T, i);
		strs[i] = str;
		len += strlen(str);
	}

	char* res = malloc(len + 1);
	res[0] = '\0';
	for (size_t i = 0; i < argc; i++) {
		strcat(res, strs[i]);
	}

	tug_ret(T, tug_str(res));
	free(strs);
}

static void __tuglib_trim(tug_Task* T) {
	const char* str = tuglib_checkstr(T, 0);
	size_t len = strlen(str);

	size_t start = 0;
	while (start < len && isspace((unsigned char)str[start])) start++;

	size_t end = len;
	while (end > start && isspace((unsigned char)str[end - 1])) end--;

	size_t newlen = end - start;
	char* res = malloc(newlen + 1);
	memcpy(res, str + start, newlen);
	res[newlen] = '\0';

	tug_ret(T, tug_str(res));
}

static void __tuglib_upper(tug_Task* T) {
	const char* str = tuglib_checkstr(T, 0);
	size_t len = strlen(str);
	char* res = malloc(len + 1);

	for (size_t i = 0; i < len; i++) {
		res[i] = toupper((unsigned char)str[i]);
	}
	res[len] = '\0';

	tug_ret(T, tug_str(res));
}

static void __tuglib_lower(tug_Task* T) {
	const char* str = tuglib_checkstr(T, 0);
	size_t len = strlen(str);
	char* res = malloc(len + 1);

	for (size_t i = 0; i < len; i++) {
		res[i] = tolower((unsigned char)str[i]);
	}
	res[len] = '\0';

	tug_ret(T, tug_str(res));
}

static void __tuglib_reverse(tug_Task* T) {
	const char* str = tuglib_checkstr(T, 0);
	size_t len = strlen(str);
	char* res = malloc(len + 1);

	for (size_t i = 0; i < len; i++) {
		res[i] = str[len - i - 1];
	}
	res[len] = '\0';

	tug_ret(T, tug_str(res));
}

static void __tuglib_repeat(tug_Task* T) {
	const char* str = tuglib_checkstr(T, 0);
	long count = tuglib_checklong(T, 1);

	if (count <= 0) {
		tug_ret(T, tug_conststr(""));
		return;
	}

	size_t len = strlen(str);
	size_t total = len * count;
	char* res = malloc(total + 1);

	char* ptr = res;
	for (size_t i = 0; i < count; i++) {
		memcpy(ptr, str, len);
		ptr += len;
	}
	*ptr = '\0';

	tug_ret(T, tug_str(res));
}

static void __tuglib_split(tug_Task* T) {
	const char* str = tuglib_checkstr(T, 0);
	const char* delim = tuglib_checkstr(T, 1);

	tug_Object* res = tug_list();
	size_t delim_len = strlen(delim);
	if (delim_len == 0) {
		for (size_t i = 0; str[i]; i++) {
			char tmp[2] = {str[i], '\0'};
			tug_listpush(res, tug_conststr(tmp));
		}
	} else {
		const char* start = str;
		const char* found;
		char* part = NULL;
		while ((found = strstr(start, delim)) != NULL) {
			size_t part_len = found - start;
			part = realloc(part, part_len + 1);
			memcpy(part, start, part_len);
			part[part_len] = '\0';
			tug_listpush(res, tug_conststr((const char*)part));

			start = found + delim_len;
		}
		free(part);

		if (*start) {
			tug_listpush(res, tug_conststr((const char*)start));
		}
	}

	tug_ret(T, res);
}

static void __tuglib_str_find(tug_Task* T) {
	const char* str = tuglib_checkstr(T, 0);
	const char* sub = tuglib_checkstr(T, 1);
	const char* pos = strstr(str, sub);

	if (pos) {
		tug_ret(T, tug_num((double)(pos - str)));
	}
}

static void __tuglib_str_replace(tug_Task* T) {
	const char* str = tuglib_checkstr(T, 0);
	const char* old = tuglib_checkstr(T, 1);
	const char* new = tuglib_checkstr(T, 2);
	long count = tuglib_optlong(T, 3, 0);

	if (!*old) {
		tug_ret(T, tug_conststr(str));
		return;
	}

	size_t old_len = strlen(old);
	size_t new_len = strlen(new);

	const char* src = str;
	size_t total_reps = 0;

	const char* tmp = str;
	while ((tmp = strstr(tmp, old))) {
		total_reps++;
		tmp += old_len;
	}
	
	if (count > 0 && (size_t)count < total_reps) {
		total_reps = count;
	}

	size_t new_size = strlen(str) + (new_len - old_len) * total_reps + 1;
	char* res = malloc(new_size);
	char* out = res;

	int done = 0;
	while (*str) {
		if ((count <= 0 || done < count) && strncmp(str, old, old_len) == 0) {
			memcpy(out, new, new_len);
			out += new_len;
			str += old_len;
			done++;
		} else {
			*out++ = *str++;
		}
	}
	*out = '\0';

	tug_ret(T, tug_str(res));
}

static void __tuglib_push(tug_Task* T) {
	tug_Object* list = tuglib_checklist(T, 0);
	tug_Object* obj = tuglib_checkany(T, 1);

	tug_listpush(list, obj);
}

static void __tuglib_pop(tug_Task* T) {
	tug_Object* list = tuglib_checklist(T, 0);
	size_t len = tug_getlen(list);
	long idx = tuglib_optlong(T, 1, len - 1);
	if (idx < 0 || idx >= len) tug_err(T, "pop index out of range");

	tug_Object* pop = tug_listpop(list, idx);
	tug_ret(T, pop);
}

static void __tuglib_insert(tug_Task* T) {
	tug_Object* list = tuglib_checklist(T, 0);
	long idx = tuglib_checklong(T, 1);
	if (idx < 0 || idx >= tug_getlen(list)) tug_err(T, "pop index out of range");
	tug_Object* obj = tuglib_checkany(T, 2);

	tug_listinsert(list, idx, obj);
}

static void __tuglib_clear(tug_Task* T) {
	tug_listclear(tuglib_checklist(T, 0));
}

static void __tuglib_unpack(tug_Task* T) {
	tug_Object* list = tuglib_checklist(T, 0);
	size_t len = tug_getlen(list);
	tug_Object* tuple = tug_tuple();
	for (size_t i = 0; i < len; i++) {
		tug_tuplepush(tuple, tug_listget(list, i));
	}

	tug_ret(T, tuple);
}

static void __tuglib_pause(tug_Task* T) {
	tug_pause(T);
}

static void tuglib_loadbuiltins(tug_Task* T) {
	tug_setglobal(T, "print", tug_cfunc("print", __tuglib_print));
	tug_setglobal(T, "tostr", tug_cfunc("tostr", __tuglib_tostr));
	tug_setglobal(T, "type", tug_cfunc("type", __tuglib_type));
	tug_setglobal(T, "setmetatable", tug_cfunc("setmetatable", __tuglib_setmetatable));
	tug_setglobal(T, "getmetatable", tug_cfunc("getmetatable", __tuglib_getmetatable));
	tug_setglobal(T, "len", tug_cfunc("len", __tuglib_len));
	tug_setglobal(T, "error", tug_cfunc("error", __tuglib_error));
	tug_setglobal(T, "pcall", tug_cfunc("pcall", __tuglib_pcall));
	tug_setglobal(T, "tonum", tug_cfunc("tonum", __tuglib_tonum));
	tug_setglobal(T, "assert", tug_cfunc("assert", __tuglib_assert));
	tug_setglobal(T, "rawget", tug_cfunc("rawget", __tuglib_rawget));
	tug_setglobal(T, "rawset", tug_cfunc("rawset", __tuglib_rawset));
	tug_setglobal(T, "clock", tug_cfunc("clock", __tuglib_clock));
	tug_setglobal(T, "pause", tug_cfunc("pause", __tuglib_pause));

	tug_Object* mathlib = tug_table();
	tug_setfield(mathlib, tug_conststr("sin"), tug_cfunc("sin", __tuglib_sin));
	tug_setfield(mathlib, tug_conststr("cos"), tug_cfunc("cos", __tuglib_cos));
	tug_setfield(mathlib, tug_conststr("tan"), tug_cfunc("tan", __tuglib_tan));
	tug_setfield(mathlib, tug_conststr("atan2"), tug_cfunc("atan2", __tuglib_atan2));
	tug_setfield(mathlib, tug_conststr("asin"), tug_cfunc("asin", __tuglib_asin));
	tug_setfield(mathlib, tug_conststr("acos"), tug_cfunc("acos", __tuglib_acos));
	tug_setfield(mathlib, tug_conststr("sqrt"), tug_cfunc("sqrt", __tuglib_sqrt));
	tug_setfield(mathlib, tug_conststr("pow"), tug_cfunc("pow", __tuglib_pow));
	tug_setfield(mathlib, tug_conststr("hypot"), tug_cfunc("hypot", __tuglib_hypot));
	tug_setfield(mathlib, tug_conststr("floor"), tug_cfunc("floor", __tuglib_floor));
	tug_setfield(mathlib, tug_conststr("ceil"), tug_cfunc("ceil", __tuglib_ceil));
	tug_setfield(mathlib, tug_conststr("round"), tug_cfunc("round", __tuglib_round));
	tug_setfield(mathlib, tug_conststr("mod"), tug_cfunc("mod", __tuglib_mod));
	tug_setfield(mathlib, tug_conststr("abs"), tug_cfunc("abs", __tuglib_abs));
	tug_setfield(mathlib, tug_conststr("seed"), tug_cfunc("seed", __tuglib_seed));
	tug_setfield(mathlib, tug_conststr("rand"), tug_cfunc("rand", __tuglib_rand));
	tug_setfield(mathlib, tug_conststr("pi"), tug_num(__tuglib_pi));
	tug_setfield(mathlib, tug_conststr("e"), tug_num(__tuglib_e));
	tug_setfield(mathlib, tug_conststr("deg"), tug_cfunc("deg", __tuglib_deg));
	tug_setfield(mathlib, tug_conststr("rad"), tug_cfunc("rad", __tuglib_rad));
	tug_setfield(mathlib, tug_conststr("log"), tug_cfunc("log", __tuglib_log));
	tug_setfield(mathlib, tug_conststr("log10"), tug_cfunc("log10", __tuglib_log10));
	tug_setfield(mathlib, tug_conststr("cbrt"), tug_cfunc("cbrt", __tuglib_cbrt));
	tug_setfield(mathlib, tug_conststr("cosh"), tug_cfunc("cosh", __tuglib_cosh));
	tug_setfield(mathlib, tug_conststr("sinh"), tug_cfunc("sinh", __tuglib_sinh));
	tug_setfield(mathlib, tug_conststr("exp"), tug_cfunc("exp", __tuglib_exp));
	tug_setfield(mathlib, tug_conststr("tanh"), tug_cfunc("tanh", __tuglib_tanh));
	tug_setfield(mathlib, tug_conststr("acosh"), tug_cfunc("acosh", __tuglib_acosh));
	tug_setfield(mathlib, tug_conststr("asinh"), tug_cfunc("asinh", __tuglib_asinh));
	tug_setfield(mathlib, tug_conststr("atanh"), tug_cfunc("atanh", __tuglib_atanh));
	tug_setfield(mathlib, tug_conststr("frexp"), tug_cfunc("frexp", __tuglib_frexp));
	tug_setfield(mathlib, tug_conststr("ldexp"), tug_cfunc("ldexp", __tuglib_ldexp));
	tug_setfield(mathlib, tug_conststr("min"), tug_cfunc("min", __tuglib_min));
	tug_setfield(mathlib, tug_conststr("max"), tug_cfunc("max", __tuglib_max));
	tug_setfield(mathlib, tug_conststr("inf"), tug_num(__tuglib_inf));
	tug_setfield(mathlib, tug_conststr("tau"), tug_num(__tuglib_tau));
	tug_setfield(mathlib, tug_conststr("nan"), tug_num(__tuglib_nan));
	tug_setglobal(T, "math", mathlib);
	
	tug_Object* strlib = tug_table();
	tug_setfield(strlib, tug_conststr("sub"), tug_cfunc("sub", __tuglib_sub));
	tug_setfield(strlib, tug_conststr("concat"), tug_cfunc("concat", __tuglib_concat));
	tug_setfield(strlib, tug_conststr("trim"), tug_cfunc("trim", __tuglib_trim));
	tug_setfield(strlib, tug_conststr("upper"), tug_cfunc("upper", __tuglib_upper));
	tug_setfield(strlib, tug_conststr("lower"), tug_cfunc("lower", __tuglib_lower));
	tug_setfield(strlib, tug_conststr("reverse"), tug_cfunc("reverse", __tuglib_reverse));
	tug_setfield(strlib, tug_conststr("repeat"), tug_cfunc("repeat", __tuglib_repeat));
	tug_setfield(strlib, tug_conststr("split"), tug_cfunc("split", __tuglib_split));
	tug_setfield(strlib, tug_conststr("find"), tug_cfunc("find", __tuglib_str_find));
	tug_setfield(strlib, tug_conststr("replace"), tug_cfunc("replace", __tuglib_str_replace));
	tug_setglobal(T, "str", strlib);
	
	tug_Object* listlib = tug_table();
	tug_setfield(listlib, tug_conststr("push"), tug_cfunc("push", __tuglib_push));
	tug_setfield(listlib, tug_conststr("pop"), tug_cfunc("pop", __tuglib_pop));
	tug_setfield(listlib, tug_conststr("insert"), tug_cfunc("insert", __tuglib_insert));
	tug_setfield(listlib, tug_conststr("clear"), tug_cfunc("clear", __tuglib_clear));
	tug_setfield(listlib, tug_conststr("unpack"), tug_cfunc("unpack", __tuglib_unpack));
	tug_setglobal(T, "list", listlib);
}

static void tuglib_loadlibs(tug_Task* T) {
	tuglib_loadbuiltins(T);
}

#endif
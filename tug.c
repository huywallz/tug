#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <math.h>
#include <unistd.h>
#include <setjmp.h>

#ifdef __ANDROID__
#include <time.h>
#endif

#ifdef __linux__
#include <sys/time.h>
#endif

#include "tug.h"
#include "tuglib.h"

#define TUG_DEBUG 1
#define TUG_CALL_LIMIT (size_t)(1000)

#define VARMAP_POOL_LIMIT 256
#define VARMAPENTRY_POOL_LIMIT 256
#define VECTOR_POOL_LIMIT 256
#define OBJ_POOL_LIMIT 4096

// Garbage collector
#define TUG_TARGET_UNTIL 0.6
#define TUG_MAX_GROWTH 2.0
#define TUG_MIN_SHRINK 0.5

#if TUG_DEBUG && !defined(__ANDROID__)

#include <execinfo.h>

void print_trace(void) {
    void *buffer[4096];
    int nptrs = backtrace(buffer, 4096);
    char **symbols = backtrace_symbols(buffer, nptrs);

    printf("Stack trace:\n");
    for (int i = 0; i < nptrs; i++) {
        printf("  %s\n", symbols[i]);
    }

    free(symbols);
}

#endif

static void* gc_malloc(size_t size);
static void* gc_realloc(void* ptr, size_t new_size);
static void gc_free(void* ptr);
static char* gc_strdup(const char* str);
static void* gc_calloc(size_t nmemb, size_t size);

static char* read_file(const char* path) {
	FILE* f = fopen(path, "rb");
	if (!f) return NULL;

	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	rewind(f);

	char* buf = gc_malloc(len + 1);
	fread(buf, len, 1, f);
	buf[len] = '\0';

	fclose(f);
	return buf;
}

static char* src;

static char emsg[2048];
static size_t eln;

static int perr(const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);
	vsnprintf(emsg, sizeof(emsg), fmt, args);
	va_end(args);

	return 1;
}

static void pprint_err(char* errmsg) {
	if (errmsg) sprintf(errmsg, "%s:%lu: %s", src, eln, emsg);
	else printf("%s:%lu: %s\n", src, eln, emsg);
}

static const char* text;
static size_t len;
static size_t idx, ln;
static char ch;

static int tkind;
static size_t tln;
static char* tstr;
static double tnum;

static size_t ldepth;

enum {
	NUM, STR, NAME, TRUE, FALSE, NIL,
	ADD, SUB, MUL, DIV, MOD,
	GT, LT, GE, LE, EQ, NE,

	AND, OR, NOT,
	IF, ELSE, ELSEIF, THEN,
	WHILE, FOR, IN, DO,
	BREAK, CONTINUE,
	FUNC, RETURN, END,

	LPAREN, RPAREN,
	LBRACK, RBRACK,
	LCURLY, RCURLY,

	LOCAL,
	ASSIGN, DOT, COMMA,

	POS, NEG,

	#if TUG_DEBUG

	DEBUG_PRINT,

	#endif

	FUNCDEF, FUNCCALL,
	TUPLE, TABLE, INDEX, SETINDEX,
	ITER_STR, ITER_TABLE,
	LIST, ITER_LIST,
};

typedef struct Node Node;
static Node* node;

static void ladv(void);
static void pinit(const char* s, const char* t) {
	src = (char*)s;
	text = t;
	len = strlen(t);
	idx = -1;
	ln = 1;
	tstr = NULL;
	node = NULL;
	ldepth = 0;
	ladv();
}

static void node_free(Node* node);
static inline void pfree(void) {
	gc_free(tstr);
	node_free(node);
}

static inline void ladv(void) {
	idx++;
	ch = idx >= len ? '\0' : text[idx];
	if (ch == '\n') ln++;
}

#define lpeek() (idx + 1 >= len ? '\0' : text[idx + 1])
#define streq(__s1, __s2) (strcmp((__s1), (__s2)) == 0)

static int ltok(void) {
	gc_free(tstr);
	tstr = NULL;
	while (isspace(ch)) ladv();
	tln = eln = ln;

	if (ch == '\0') {
		tkind = EOF;
		return 0;
	}

	if (isdigit(ch) || (ch == '.' && isdigit(lpeek()))) {
		int dot = ch == '.';
		size_t start = idx;

		while (isdigit(ch) || ch == '.') {
			if (ch == '.') {
				if (dot) break;
				dot++;
			}
			ladv();
		}

		size_t len = idx - start;
		char* s = gc_malloc(len + 1);
		memcpy(s, &text[start], len);
		s[len] = '\0';

		char* endptr;
		tnum = strtod(s, &endptr);
		if (*endptr != '\0') {
			gc_free(s);
			return perr("malformed number");
		}
		gc_free(s);

		tkind = NUM;
		return 0;
	}

	if (ch == '"' || ch == '\'') {
		tkind = STR;
		char del = ch;
		ladv();
		
		tstr = gc_malloc(1);
		size_t len = 0;
		
		while (ch != del && ch != '\0' && ch != '\n') {
			char c;
			if (ch == '\\') {
				ladv();
				switch (ch) {
					case '\\': c = '\\'; break;
					case '\'': c = '\''; break;
					case '"': c = '\"'; break;
					case 'n': c = '\n'; break;
					case 't': c = '\t'; break;
					default: return perr("invalid escape character '\\%c'", ch);
				}
			} else c = ch;
			tstr = gc_realloc(tstr, len + 2);
			tstr[len++] = c;
			ladv();
		}
		tstr[len] = '\0';

		if (ch != del) return perr("unfinished string");
		ladv();
		return 0;
	}

	if (isalpha(ch) || ch == '_') {
		size_t start = idx;

		while (isalnum(ch) || ch == '_') ladv();

		size_t len = idx - start;
		tstr = gc_malloc(len + 1);
		memcpy(tstr, &text[start], len);
		tstr[len] = '\0';

		if (streq(tstr, "true")) tkind = TRUE;
		else if (streq(tstr, "false")) tkind = FALSE;
		else if (streq(tstr, "nil")) tkind = NIL;
		else if (streq(tstr, "and")) tkind = AND;
		else if (streq(tstr, "or")) tkind = OR;
		else if (streq(tstr, "not")) tkind = NOT;
		else if (streq(tstr, "if")) tkind = IF;
		else if (streq(tstr, "else")) tkind = ELSE;
		else if (streq(tstr, "elseif")) tkind = ELSEIF;
		else if (streq(tstr, "then")) tkind = THEN;
		else if (streq(tstr, "while")) tkind = WHILE;
		else if (streq(tstr, "for")) tkind = FOR;
		else if (streq(tstr, "in")) tkind = IN;
		else if (streq(tstr, "do")) tkind = DO;
		else if (streq(tstr, "break")) tkind = BREAK;
		else if (streq(tstr, "continue")) tkind = CONTINUE;
		else if (streq(tstr, "func")) tkind = FUNC;
		else if (streq(tstr, "return")) tkind = RETURN;
		else if (streq(tstr, "end")) tkind = END;

		#if TUG_DEBUG

		else if (streq(tstr, "debug_print")) tkind = DEBUG_PRINT;

		#endif

		else tkind = NAME;

		return 0;
	}

	switch (ch) {
		case '+': tkind = ADD; break;
		case '-': tkind = SUB; break;
		case '*': tkind = MUL; break;
		case '/': tkind = DIV; break;
		case '%': tkind = MOD; break;
		case '(': tkind = LPAREN; break;
		case ')': tkind = RPAREN; break;
		case '[': tkind = LBRACK; break;
		case ']': tkind = RBRACK; break;
		case '{': tkind = LCURLY; break;
		case '}': tkind = RCURLY; break;
		case '.': tkind = DOT; break;
		case ',': tkind = COMMA; break;
		case '>': {
			if (lpeek() == '=') {
				ladv();
				tkind = GE;
			} else tkind = GT;
		} break;
		case '<': {
			if (lpeek() == '=') {
				ladv();
				tkind = LE;
			} else tkind = LT;
		} break;
		case '=': {
			if (lpeek() == '=') {
				ladv();
				tkind = EQ;
			} else tkind = ASSIGN;
		} break;
		case '!': {
			if (lpeek() == '=') {
				ladv();
				tkind = NE;
			} else return perr("unexpected symbol '!'");
		} break;
		case ':': {
			if (lpeek() == '=') {
				ladv();
				tkind = LOCAL;
			} else return perr("unexpected symbol ':'");
		} break;
		default: {
			if (isprint((unsigned char)ch)) return perr("unexpected symbol '%c'", ch);
			else return perr("unexpected symbol (%d)", (int)ch);
		}
	}

	ladv();
	return 0;
}

static int lpeektk(int* type) {
	size_t pidx = idx;
	size_t pln = ln;

	int err = ltok();
	if (err) return err;

	if (type) *type = tkind;
	idx = pidx;
	ln = pln;

	return 0;
}

typedef struct {
	void** array;
	size_t capacity;
	size_t count;
	uint8_t pooled;
} Vector;

static Vector* vec_pool[VECTOR_POOL_LIMIT];
static size_t vec_poolc = 0;

static void vec_clearpool(void) {
	for (size_t i = 0; i < vec_poolc; i++) {
		Vector* vec = vec_pool[i];
		gc_free(vec->array);
		gc_free(vec);
	}
	vec_poolc = 0;
}

static Vector* vec_serve(size_t size) {
	if (size < 8) size = 8;
	if (vec_poolc > 0) {
		Vector* res = vec_pool[--vec_poolc];
		res->count = 0;
		
		if (res->capacity < size) {
			res->capacity = size;
			res->array = gc_realloc(res->array, res->capacity * sizeof(void*));
		}
		res->pooled = 0;

		return res;
	}

	Vector* vec = gc_malloc(sizeof(Vector));
	vec->capacity = size;
	vec->count = 0;
	vec->array = gc_malloc(vec->capacity * sizeof(void*));
	vec->pooled = 0;

	return vec;
}

static Vector* vec_create() {
	return vec_serve(64);
}

static void vec_dynamic(Vector* vec) {
	if (vec->count >= vec->capacity) {
		vec->capacity *= 2;
		vec->array = gc_realloc(vec->array, vec->capacity * sizeof(void*));
	} else if (vec->count < vec->capacity / 4 && vec->capacity > 8) {
		vec->capacity /= 2;
		vec->array = gc_realloc(vec->array, vec->capacity * sizeof(void*));
	}
}

static void vec_push(Vector* vec, void* obj) {
	vec_dynamic(vec);
	vec->array[vec->count++] = obj;
}

static void vec_pushfirst(Vector* vec, void* obj) {
	if (vec->count >= vec->capacity) {
		vec->capacity *= 2;
		vec->array = gc_realloc(vec->array, vec->capacity * sizeof(void*));
	}

	memmove(&vec->array[1], &vec->array[0], vec->count * sizeof(void*));
	vec->array[0] = obj;
	vec->count++;
}

static void* vec_pop(Vector* vec) {
	if (vec->count == 0) return NULL;
	void* res = vec->array[--vec->count];
	vec_dynamic(vec);

	return res;
}

static void* vec_popfirst(Vector* vec) {
	if (vec->count == 0) return NULL;
	void* res = vec->array[0];

	memmove(&vec->array[0], &vec->array[1], (vec->count - 1) * sizeof(void*));
	vec->count--;

	if (vec->count < vec->capacity / 4 && vec->capacity > 8) {
		vec->capacity /= 4;
		vec->array = gc_realloc(vec->array, vec->capacity * sizeof(void*));
	}
	return res;
}

#define vec_set(__vec, __idx, __val) ((__vec)->array[__idx] = (__val))

// required manual free inside vector before call `vec_free`
static void vec_free(Vector* vec) {
	if (!vec || vec->pooled) return;
	if (vec_poolc < VECTOR_POOL_LIMIT) {
		vec_pool[vec_poolc++] = vec;
		vec->pooled = 1;
	} else {
		gc_free(vec->array);
		gc_free(vec);
	}
}

#define vec_peek(v) ((v)->count == 0 ? NULL : (v)->array[(v)->count - 1])
#define vec_count(v) ((v) ? (v)->count : 0)
#define vec_get(v, i) (v)->array[i]

static inline void __vec_iter(Vector* vec, void (*iterfunc)(void*)) {
	for (size_t i = 0; i < vec_count(vec); i++) {
		iterfunc(vec_get(vec, i));
	}
}

// iterating vector and call function with pointer
#define vec_iter(v, f) __vec_iter((v), (void(*)(void*))(f))

static inline void __vec_advfree(Vector* vec, void (*freefunc)(void*)) {
	vec_iter(vec, freefunc);
	vec_free(vec);
}

// advanced free i think
#define vec_advfree(v, f) __vec_advfree((v), (void(*)(void*))(f))

// freeing vector and vector items using standard `free`
#define vec_stdfree(__vec) vec_advfree((__vec), gc_free)

typedef struct Node {
	int kind;
	void* data;
} Node;

static Node* node_create(int kind, void* data) {
	Node* res = gc_malloc(sizeof(Node));
	res->kind = kind;
	res->data = data;

	return res;
}

typedef struct {
	Node* o1;
	Node* o2;
	size_t ln;
} Node_BinOp;

static Node* node_binop(int kind, Node* o1, Node* o2, size_t ln) {
	Node_BinOp* data = gc_malloc(sizeof(Node_BinOp));
	data->o1 = o1;
	data->o2 = o2;
	data->ln = ln;

	return node_create(kind, data);
}

typedef struct {
	Node* right;
	size_t ln;
} Node_Unary;

static Node* node_unary(int kind, Node* right, size_t ln) {
	Node_Unary* data = gc_malloc(sizeof(Node_Unary));
	data->right = right;
	data->ln = ln;

	return node_create(kind, data);
}

typedef struct {
	char* str;
} Node_Str;

static Node* node_str(int kind, const char* str) {
	Node_Str* data = gc_malloc(sizeof(Node_Str));
	data->str = gc_strdup(str);

	return node_create(kind, data);
}

typedef struct {
	double num;
} Node_Num;

static Node* node_num(int kind, double num) {
	Node_Num* data = gc_malloc(sizeof(Node_Num));
	data->num = num;

	return node_create(kind, data);
}

#if TUG_DEBUG

typedef struct {
	Node* expr;
} Node_DebugPrint;

static Node* node_debugprint(Node* expr) {
	Node_DebugPrint* debugprint = gc_malloc(sizeof(Node_DebugPrint));
	debugprint->expr = expr;

	return node_create(DEBUG_PRINT, debugprint);
}

#endif

typedef struct {
	Node** nodes;
	size_t count;
	size_t capacity;
} NodeBlock;

static NodeBlock* node_block(void) {
	NodeBlock* block = gc_malloc(sizeof(NodeBlock));
	block->count = 0;
	block->capacity = 64;
	block->nodes = gc_malloc(sizeof(Node*) * block->capacity);

	return block;
}

static void node_block_push(NodeBlock* block, Node* node) {
	if (block->capacity >= block->count) {
		block->capacity *= 2;
		block->nodes = gc_realloc(block->nodes, sizeof(Node*) * block->capacity);
	}

	block->nodes[block->count++] = node;
}

static void node_free(Node* node);
static void node_block_free(NodeBlock* block) {
	if (!block) return;

	for (size_t i = 0; i < block->count; i++) {
		node_free(block->nodes[i]);
	}

	gc_free(block->nodes);
	gc_free(block);
}

typedef struct {
	Node* cond;
	NodeBlock* block;
	Vector* conds;
	Vector* blocks;
	NodeBlock* eblock;
} Node_If;

static Node* node_if(Node* cond, NodeBlock* block, Vector* conds, Vector* blocks, NodeBlock* eblock) {
	Node_If* nif = gc_malloc(sizeof(Node_If));
	nif->cond = cond;
	nif->block = block;
	nif->conds = conds;
	nif->blocks = blocks;
	nif->eblock = eblock;

	return node_create(IF, nif);
}

typedef struct {
	Node* cond;
	NodeBlock* block;
} Node_While;

static Node* node_while(Node* cond, NodeBlock* block) {
	Node_While* nwhile = gc_malloc(sizeof(Node_While));
	nwhile->cond = cond;
	nwhile->block = block;

	return node_create(WHILE, nwhile);
}

typedef struct {
	Vector* names;
	Vector* params;
	NodeBlock* block;
	size_t ln;
} Node_FuncDef;

static Node* node_funcdef(Vector* names, Vector* params, NodeBlock* block, size_t ln) {
	Node_FuncDef* funcdef = gc_malloc(sizeof(Node_FuncDef));
	funcdef->names = names;
	funcdef->params = params;
	funcdef->block = block;
	funcdef->ln = ln;

	return node_create(FUNCDEF, funcdef);
}

typedef struct {
	Node* node;
	Vector* values;
	size_t ln;
} Node_FuncCall;

static Node* node_funccall(Node* node, Vector* values, size_t ln) {
	Node_FuncCall* funccall = gc_malloc(sizeof(Node_FuncCall));
	funccall->node = node;
	funccall->values = values;
	funccall->ln = ln;

	return node_create(FUNCCALL, funccall);
}

typedef struct {
	Vector* keys;
	Vector* values;
} Node_Table;

static Node* node_table(Vector* keys, Vector* values) {
	Node_Table* ntable = gc_malloc(sizeof(Node_Table));
	ntable->keys = keys;
	ntable->values = values;

	return node_create(TABLE, ntable);
}

typedef struct {
	Vector* names;
	Node* node;
	NodeBlock* block;
	size_t ln;
} Node_For;

static Node* node_for(Vector* names, Node* node, NodeBlock* block, size_t ln) {
	Node_For* nfor = gc_malloc(sizeof(Node_For));
	nfor->names = names;
	nfor->node = node;
	nfor->block = block;
	nfor->ln = ln;

	return node_create(FOR, nfor);
}

typedef struct {
	int kind;
	char* name;
	Node* obj;
	Node* key;
} Assign;

static Assign* assign_var(char* name) {
	Assign* assign = gc_malloc(sizeof(Assign));
	assign->kind = ASSIGN;
	assign->name = name;

	return assign;
}

static Assign* assign_index(Node* obj, Node* key) {
	Assign* assign = gc_malloc(sizeof(Assign));
	assign->kind = SETINDEX;
	assign->obj = obj;
	assign->key = key;

	return assign;
}

static void assign_free(Assign* assign) {
	if (assign->kind == ASSIGN) gc_free(assign->name);
	else {
		node_free(assign->obj);
		node_free(assign->key);
	}

	gc_free(assign);
}

typedef struct {
	Vector* assigns;
	uint8_t local;
	Vector* values;
	size_t ln;
} Node_Assignment;

static Node* node_assignment(Vector* assigns, uint8_t local, Vector* values, size_t ln) {
	Node_Assignment* assignment = gc_malloc(sizeof(Node_Assignment));
	assignment->assigns = assigns;
	assignment->local = local;
	assignment->values = values;
	assignment->ln = ln;

	return node_create(ASSIGN, assignment);
}

static void node_free(Node* node) {
	if (!node) return;

	switch (node->kind) {
		case ADD:
		case SUB:
		case MUL:
		case DIV:
		case MOD:
		case GT:
		case LT:
		case GE:
		case LE:
		case EQ:
		case NE:
		case AND:
		case OR:
		case INDEX: {
			Node_BinOp* binop = (Node_BinOp*)node->data;
			node_free(binop->o1);
			node_free(binop->o2);
		} break;

		case POS:
		case NEG:
		case NOT: {
			Node_Unary* unary = (Node_Unary*)node->data;
			node_free(unary->right);
		} break;

		case STR:
		case NAME: {
			Node_Str* str = (Node_Str*)node->data;
			gc_free(str->str);
		} break;

		#if TUG_DEBUG

		case DEBUG_PRINT: {
			Node_DebugPrint* debugprint = (Node_DebugPrint*)node->data;
			node_free(debugprint->expr);
		} break;

		#endif

		case IF: {
			Node_If* nif = (Node_If*)node->data;

			node_free(nif->cond);

			node_block_free(nif->block);
			vec_advfree(nif->conds, node_free);
			vec_advfree(nif->blocks, node_block_free);

			node_block_free(nif->eblock);
		} break;

		case WHILE: {
			Node_While* nwhile = (Node_While*)node->data;

			node_free(nwhile->cond);
			node_block_free(nwhile->block);
		} break;

		case FUNCDEF: {
			Node_FuncDef* funcdef = (Node_FuncDef*)node->data;

			vec_stdfree(funcdef->names);
			vec_stdfree(funcdef->params);

			node_block_free(funcdef->block);
		} break;

		case FUNCCALL: {
			Node_FuncCall* funccall = (Node_FuncCall*)node->data;

			node_free(funccall->node);
			vec_advfree(funccall->values, node_free);
		} break;

		case LIST:
		case RETURN: {
			vec_advfree(node->data, node_free);
			node->data = NULL;
		} break;

		case TABLE: {
			if (node->data) {
				Node_Table* ntable = (Node_Table*)node->data;
				vec_advfree(ntable->keys, node_free);
				vec_advfree(ntable->values, node_free);
			}
		} break;

		case FOR: {
			Node_For* nfor = (Node_For*)node->data;
			vec_stdfree(nfor->names);
			node_free(nfor->node);
			node_block_free(nfor->block);
		} break;

		case ASSIGN: {
			Node_Assignment* assignment = (Node_Assignment*)node->data;
			vec_advfree(assignment->assigns, assign_free);
			vec_advfree(assignment->values, node_free);
		} break;
	}

	gc_free(node->data);
	gc_free(node);
}

static int node_isexpr(Node* node) {
	switch (node->kind) {
		case ADD:
		case SUB:
		case MUL:
		case DIV:
		case MOD:
		case GT:
		case LT:
		case GE:
		case LE:
		case EQ:
		case NE:
		case AND:
		case OR:
		case POS:
		case NEG:
		case NOT:
		case STR:
		case NUM:
		case NAME:
		case FUNCCALL:
		case INDEX:
		case LIST:
		return 1;
		default:
		return 0;
	}
}

#define pexpr por
static int por(void);
static NodeBlock* pblock(int elseif);
static int pval(void) {
	if (tkind == STR || tkind == NAME) {
		node = node_str(tkind, (const char*)tstr);
		return ltok();
	} else if (tkind == NUM) {
		node = node_num(tkind, tnum);
		return ltok();
	} else if (tkind == TRUE || tkind == FALSE || tkind == NIL) {
		node = node_create(tkind, NULL);
		return ltok();
	} else if (tkind == LPAREN) {
		if (ltok() || pexpr()) return 1;
		else if (tkind != RPAREN) return perr("expected ')'");

		return ltok();
	} else if (tkind == LCURLY) {
		if (ltok()) return 1;
		if (tkind == RCURLY) {
			node = node_create(TABLE, NULL);

			return ltok();
		}

		Vector* keys = vec_create();
		Vector* values = vec_create();
		while (tkind != RCURLY && tkind != EOF) {
			if (tkind == LBRACK) {
				if (ltok() || pexpr()) goto __terr;
				else if (tkind != RBRACK) {
					perr("expected ']'");
					goto __terr;
				} else if (ltok()) goto __terr;
				else if (tkind != ASSIGN) {
					perr("expected '='");
					goto __terr;
				} else if (ltok()) goto __terr;

				vec_push(keys, node);
				if (pexpr()) goto __terr;
				vec_push(values, node);
				node = NULL;
				if (ltok()) goto __terr;
			} else if (tkind == NAME) {
				vec_push(keys, node_str(STR, (const char*)tstr));
				int kind;
				if (lpeektk(&kind)) goto __terr;

				if (kind == ASSIGN) {
					ltok();
					if (ltok() || pexpr()) goto __terr;
					vec_push(values, node);
					node = NULL;
				} else {
					node_free(vec_pop(keys));
					if (pexpr()) goto __terr;
					vec_push(keys, NULL);
					vec_push(values, node);
					node = NULL;
				}
				node = NULL;
			} else {
				if (pexpr()) goto __terr;
				vec_push(keys, NULL);
				vec_push(values, node);
				node = NULL;
			}

			if (tkind == COMMA) {
				if (ltok()) goto __terr;
			} else if (tkind != RCURLY && tkind != EOF) {
				perr("expected ',' or '}'");
				goto __terr;
			}
		}

		if (tkind != RCURLY) {
			perr("expected '}'");
			goto __terr;
		}

		node = node_table(keys, values);
		return ltok();

		__terr: {
			vec_advfree(keys, node_free);
			vec_advfree(values, node_free);

			return 1;
		}
	} else if (tkind == FUNC) {
		size_t ln = tln;
		Vector* params = NULL;
		if (ltok()) return 1;
		else if (tkind != LPAREN) return perr("expected '('");
		else if (ltok()) return 1;
		else if (tkind != RPAREN) {
			params = vec_create();
			
			while (1) {
				if (tkind != NAME) {
					vec_stdfree(params);
					return perr("expected '<name>'");
				}
				vec_push(params, gc_strdup(tstr));
				
				if (ltok()) {
					vec_stdfree(params);
					return 1;
				} else if (tkind == COMMA) {
					if (ltok()) {
						vec_stdfree(params);
						return 1;
					}
				} else break;
			}
		}
		if (tkind != RPAREN) {
			vec_stdfree(params);
			return perr("expected ')'");
		} else if (ltok()) {
			vec_stdfree(params);
			return 1;
		}
		
		NodeBlock* block = pblock(0);
		if (!block) {
			vec_stdfree(params);
			return 1;
		}
		
		node = node_funcdef(NULL, params, block, ln);
		return ltok();
	} else if (tkind == LBRACK) {
		if (ltok()) return 1;
		if (tkind == RBRACK) {
			node = node_create(LIST, NULL);
			return ltok();
		}
		
		Vector* nodes = vec_create();
		while (1) {
			if (pexpr()) {
				vec_advfree(nodes, node_free);
				return 0;
			}
			vec_push(nodes, node);
			
			if (tkind == COMMA) {
				if (ltok()) {
					vec_advfree(nodes, node_free);
					return 0;
				}
			} else if (tkind != RBRACK) {
				vec_advfree(nodes, node_free);
				return perr("expected ',' or ']'");
			}
			
			if (tkind == RBRACK) {
				break;
			}
		}
		
		node = node_create(LIST, nodes);
		return ltok();
	}

	return perr("unexpected token");
}

static int pcall(void) {
	if (pval()) return 1;
	Node* left = node;
	node = NULL;

	while (tkind == LPAREN || tkind == LBRACK || tkind == DOT) {
		size_t ln = tln;
		int kind = tkind;
		if (ltok()) {
			node_free(left);
			return 1;
		}

		if (kind == LPAREN) {
			Vector* values = NULL;
			#define pcall_free() vec_advfree(values, node_free); node_free(left)
			if (tkind != RPAREN) {
				values = vec_create();

				while (1) {
					if (pexpr()) {
						pcall_free();
						return 1;
					}

					vec_push(values, node);
					node = NULL;

					if (tkind != COMMA) break;
					else if (ltok()) {
						pcall_free();
						return 1;
					}
				}
			}

			if (tkind != RPAREN) {
				pcall_free();
				return perr("expected ')'");
			} else if (ltok()) {
				pcall_free();
				return 1;
			}

			left = node_funccall(left, values, ln);
		} else if (kind == LBRACK) {
			if (pexpr()) {
				node_free(left);
				return 1;
			}

			if (tkind != RBRACK) {
				node_free(left);
				return perr("expected ']'");
			} else if (ltok()) {
				node_free(left);
				return 1;
			}

			left = node_binop(INDEX, left, node, ln);
		} else if (kind == DOT) {
			if (tkind != NAME) {
				node_free(left);
				return perr("expected '<name>'");
			}

			Node* key = node_str(STR, (const char*)tstr);
			if (ltok()) {
				node_free(key);
				node_free(left);
				return 1;
			}

			left = node_binop(INDEX, left, key, ln);
		}
	}

	node = left;
	return 0;
}

static int punary(void) {
	if (tkind == ADD || tkind == SUB || tkind == NOT) {
		int kind = (tkind == ADD ? POS : tkind == SUB ? NEG : NOT);
		size_t ln = tln;
		if (ltok() || punary()) return 1;
		node = node_unary(kind, node, ln);

		return 0;
	}

	return pcall();
}

static int pterm(void) {
	if (punary()) return 1;
	Node* left = node;

	while (tkind == MUL || tkind == DIV || tkind == MOD) {
		int kind = tkind;
		size_t ln = tln;
		if (ltok() || punary()) return 1;
		Node* right = node;
		left = node_binop(kind, left, right, ln);
	}
	node = left;

	return 0;
}

static int parith(void) {
	if (pterm()) return 1;
	Node* left = node;

	while (tkind == ADD || tkind == SUB) {
		int kind = tkind;
		size_t ln = tln;
		if (ltok() || pterm()) return 1;
		Node* right = node;
		left = node_binop(kind, left, right, ln);
	}
	node = left;

	return 0;
}

static int pcomp(void) {
	if (parith()) return 1;
	Node* left = node;

	while (tkind == GT || tkind == LT || tkind == GE || tkind == LE || tkind == EQ || tkind == NE) {
		int kind = tkind;
		size_t ln = tln;
		if (ltok() || parith()) return 1;
		Node* right = node;
		left = node_binop(kind, left, right, ln);
	}
	node = left;

	return 0;
}

static int pand(void) {
	if (pcomp()) return 1;
	Node* left = node;

	while (tkind == AND) {
		size_t ln = tln;
		if (ltok() || pcomp()) return 1;
		Node* right = node;
		left = node_binop(AND, left, right, ln);
	}
	node = left;

	return 0;
}

static int por(void) {
	if (pand()) return 1;
	Node* left = node;

	while (tkind == OR) {
		size_t ln = tln;
		if (ltok() || pand()) return 1;
		Node* right = node;
		left = node_binop(OR, left, right, ln);
	}
	node = left;

	return 0;
}

static int pstmt(void);
static NodeBlock* pblock(int elseif) {
	NodeBlock* block = node_block();
	while (tkind != END && tkind != EOF && (!elseif || (tkind != ELSEIF && tkind != ELSE))) {
		if (pstmt()) {
			node_block_free(block);
			return NULL;
		}
		node_block_push(block, node);
		node = NULL;
	}

	if (!elseif && tkind != END) {
		node_block_free(block);
		perr("expected 'end'");
		return NULL;
	}

	return block;
}

static int pstmt(void) {
	#if TUG_DEBUG

	if (tkind == DEBUG_PRINT) {
		if (ltok() || pexpr()) return 1;
		node = node_debugprint(node);

		return 0;
	}

	#endif

	if (tkind == IF) {
		if (ltok() || pexpr()) return 1;
		else if (tkind != THEN) return perr("expected 'then'");
		else if (ltok()) return 1;

		Node* cond = node;
		node = NULL;

		NodeBlock* block = pblock(1);
		if (!block) {
			node_free(cond);
			return 1;
		}

		Vector* conds = vec_create();
		Vector* blocks = vec_create();
		while (tkind == ELSEIF) {
			if (ltok() || pexpr()) goto __err;
			else if (tkind != THEN) {
				perr("expected 'then'");
				goto __err;
			} else if (ltok()) goto __err;

			Node* econd = node;
			node = NULL;

			NodeBlock* block = pblock(1);
			if (!block) {
				node_free(econd);
				goto __err;
			}

			vec_push(conds, econd);
			vec_push(blocks, block);
		}

		NodeBlock* eblock = NULL;
		if (tkind == ELSE) {
			if (ltok()) goto __err;
			eblock = pblock(0);
			if (!eblock) goto __err;
		} else if (tkind != END) {
			node_block_free(eblock);
			perr("expected 'end'");
			goto __err;
		}

		node = node_if(cond, block, conds, blocks, eblock);
		return ltok();

		__err: {
			node_free(cond);
			node_block_free(block);
			vec_advfree(conds, node_free);
			vec_advfree(blocks, node_block_free);

			return 1;
		}
	}

	if (tkind == WHILE) {
		if (ltok() || pexpr()) return 1;
		else if (tkind != DO) return perr("expected 'do'");
		else if (ltok()) return 1;

		Node* cond = node;
		node = NULL;

		ldepth++;
		NodeBlock* block = pblock(0);
		ldepth--;
		if (!block) {
			node_free(cond);
			return 1;
		}

		node = node_while(cond, block);
		return ltok();
	}

	if (tkind == BREAK || tkind == CONTINUE) {
		if (ldepth == 0) {
			if (tkind == BREAK) return perr("'break' outside loop");
			else return perr("'continue' outside loop");
		}

		node = node_create(tkind, NULL);
		return ltok();
	}

	if (tkind == FUNC) {
		size_t ln = tln;
		if (ltok()) return 1;
		else if (tkind != NAME) return perr("expected '<name>'");

		Vector* names = vec_create();
		vec_push(names, gc_strdup(tstr));
		if (ltok()) goto __freename;
		while (tkind == DOT) {
			if (ltok()) goto __freename;
			if (tkind != NAME) {
				perr("expected '<name>'");
				goto __freename;
			}
			vec_push(names, gc_strdup(tstr));
			if (ltok()) goto __freename;
		}

		if (tkind != LPAREN) {
			perr("expected '('");
			goto __freename;
		} else if (ltok()) goto __freename;

		Vector* params = NULL;
		goto __donename;

		__freename: {
			vec_stdfree(names);
			return 1;
		}

		__donename:

		if (tkind != RPAREN) {
			params = vec_create();
			while (1) {
				if (tkind != NAME) {
					perr("expected '<name>'");
					goto __freeparams;
				}

				char* param = gc_strdup(tstr);
				vec_push(params, param);
				if (ltok()) goto __freeparams;

				if (tkind != COMMA) break;
				else if (ltok()) goto __freeparams;
			}
		}
		goto __nofreeparams;

		__freeparams: {
			vec_stdfree(names);
			vec_stdfree(params);

			return 1;
		}

		__nofreeparams:

		if (tkind != RPAREN) {
			perr("expected ')'");
			goto __freeparams;
		} else if (ltok()) goto __freeparams;

		NodeBlock* block = pblock(0);
		if (!block) goto __freeparams;

		node = node_funcdef(names, params, block, ln);
		return ltok();
	}

	if (tkind == RETURN) {
		if (ltok()) return 1;
		if (tkind == END || tkind == ELSEIF || tkind == ELSE || tkind == EOF) {
			node = node_create(RETURN, NULL);
			return 0;
		}

		Vector* values = vec_create();
		while (1) {
			if (pexpr()) {
				vec_advfree(values, node_free);
				return 1;
			}
			vec_push(values, node);
			node = NULL;

			if (tkind == COMMA) {
				if (ltok()) {
					vec_advfree(values, node_free);
					return 1;
				}
				continue;
			}

			break;
		}

		node = node_create(RETURN, values);
		return 0;
	}

	if (tkind == FOR) {
		size_t ln = tln;
		if (ltok()) return 1;
		if (tkind != NAME) return perr("expected '<name>'");

		Vector* names = vec_create();
		while (1) {
			vec_push(names, gc_strdup(tstr));
			if (ltok()) goto __forerr;

			if (tkind == COMMA) {
				if (ltok()) goto __forerr;
				if (tkind != NAME) {
					perr("expected '<name>'");
					goto __forerr;
				}
			} else break;
		}

		if (tkind != IN) {
			perr("expected 'in'");
			goto __forerr;
		} else if (ltok() || pexpr()) goto __forerr;
		else if (tkind != DO) {
			perr("expected 'do'");
			goto __forerr;
		} else if (ltok()) goto __forerr;

		Node* obj = node;
		node = NULL;

		NodeBlock* block = pblock(0);
		if (!block) {
			node_free(obj);
			goto __forerr;
		}

		node = node_for(names, obj, block, ln);
		return ltok();

		__forerr: {
			vec_stdfree(names);
			return 1;
		}
	}

	if (pexpr()) return 1;

	if ((node->kind == NAME || node->kind == INDEX) && (tkind == LOCAL || tkind == ASSIGN || tkind == COMMA)) {
		Vector* assigns = vec_create();

		#define __push_new_assign() { \
			if (node->kind == NAME) { \
				Node_Str* str = (Node_Str*)node->data; \
				vec_push(assigns, assign_var(str->str)); \
				gc_free(str); \
			} else { \
				Node_BinOp* binop = (Node_BinOp*)node->data; \
				vec_push(assigns, assign_index(binop->o1, binop->o2)); \
				gc_free(binop); \
			} \
			gc_free(node); \
			node = NULL; \
		}

		__push_new_assign();

		int must_assign = 0;

		while (1) {
			if (tkind == COMMA) {
				if (ltok()) goto __lefterr;
			} else if (tkind == ASSIGN || tkind == LOCAL) break;

			if (pexpr()) goto __lefterr;
			if (node->kind != NAME && node->kind != INDEX) {
				perr("invalid assignment target");
				goto __lefterr;
			}
			if (node->kind == INDEX) must_assign = 1;
			__push_new_assign();
		}

		uint8_t local = tkind == LOCAL;
		size_t ln = tln;
		if (must_assign && local) {
			perr("invalid ':=' (expected '=')");
			goto __lefterr;
		}
		if (ltok()) goto __lefterr;

		Vector* values = vec_create();
		while (1) {
			if (pexpr()) goto __fullerr;
			vec_push(values, node);
			node = NULL;

			if (tkind == COMMA) {
				if (ltok()) goto __fullerr;
			} else break;
		}

		node = node_assignment(assigns, local, values, ln);
		return 0;

		__fullerr: {
			vec_advfree(values, node_free);
		}

		__lefterr: {
			vec_advfree(assigns, assign_free);
			return 1;
		}
	}

	return 0;
}

typedef struct {
	uint8_t* data;
	size_t capacity;
	size_t size;
	int ref;
} Bytecode;

static Bytecode* main_bc;

static Bytecode* bc_create() {
	Bytecode* bc = gc_malloc(sizeof(Bytecode));
	bc->capacity = 128;
	bc->size = 0;
	bc->data = gc_malloc(bc->capacity);
	bc->ref = 0;

	return bc;
}

static void bc_free(Bytecode* bc) {
	if (!bc) return;
	bc->ref--;
	if (bc->ref <= 0) {
		gc_free(bc->data);
		gc_free(bc);
	}
}

static void ensure(size_t extra) {
	while (main_bc->size + extra > main_bc->capacity) {
		main_bc->capacity *= 2;
	}
	main_bc->data = gc_realloc(main_bc->data, main_bc->capacity);
}

static void emit_byte(uint8_t value) {
	ensure(1);
	main_bc->data[main_bc->size++] = value;
}

static void emit_num(double num) {
	ensure(sizeof(double));
	memcpy(&main_bc->data[main_bc->size], &num, sizeof(double));
	main_bc->size += sizeof(double);
}

static void emit_str(const char* str) {
	size_t len = strlen(str) + 1;
	ensure(len);
	memcpy(&main_bc->data[main_bc->size], str, len);
	main_bc->size += len;
}

static size_t emit_addr(size_t addr) {
	size_t pos = main_bc->size;
	ensure(sizeof(size_t));
	memcpy(&main_bc->data[main_bc->size], &addr, sizeof(size_t));
	main_bc->size += sizeof(size_t);

	return pos;
}

static size_t emit_jump(uint8_t op, size_t addr, uint8_t pback) {
	ensure(sizeof(uint8_t) * 2 + sizeof(size_t));
	emit_byte(op);
	size_t pos = emit_addr(addr);
	emit_byte(pback);

	return pos;
}

static void patch_addr(size_t pos, size_t addr) {
	memcpy(&main_bc->data[pos], &addr, sizeof(size_t));
}

static void emit_bc(Bytecode* bc) {
	emit_addr(bc->size);
	ensure(bc->size);
	memcpy(&main_bc->data[main_bc->size], bc->data, bc->size);
	main_bc->size += bc->size;
}

// used for small things like storing object kinds (types)
typedef struct {
	uint8_t* values;
	size_t count;
	size_t capacity;
} ui8_array;

static ui8_array* ui8_array_create(void) {
	ui8_array* array = gc_malloc(sizeof(ui8_array));
	array->count = 0;
	array->capacity = 8;
	array->values = gc_malloc(array->capacity * sizeof(uint8_t));

	return array;
}

static void ui8_array_push(ui8_array* array, uint8_t value) {
	if (array->count >= array->capacity) {
		array->capacity *= 2;
		array->values = gc_realloc(array, array->capacity * sizeof(uint8_t));
	}

	array->values[array->count++] = value;
}

static uint8_t ui8_array_get(ui8_array* array, size_t index) {
	return array->values[index];
}

static void ui8_array_free(ui8_array* array) {
	gc_free(array->values);
	gc_free(array);
}

typedef struct {
	size_t* poses;
	size_t count;
	size_t capacity;
} pos_stack;

static pos_stack* pos_stack_create(void) {
	pos_stack* stack = gc_malloc(sizeof(pos_stack));
	stack->count = 0;
	stack->capacity = 16;
	stack->poses = gc_malloc(sizeof(size_t) * stack->capacity);

	return stack;
}

static void pos_stack_push(pos_stack* stack, size_t pos) {
	if (stack->capacity >= stack->count) {
		stack->capacity *= 2;
		stack->poses = gc_realloc(stack->poses, sizeof(size_t) * stack->capacity);
	}

	stack->poses[stack->count++] = pos;
}

static size_t pos_stack_pop(pos_stack* stack) {
	return stack->poses[--stack->count];
}

static int pos_stack_empty(pos_stack* stack) {
	return stack->count == 0;
}

static void pos_stack_free(pos_stack* stack) {
	gc_free(stack->poses);
	gc_free(stack);
}

typedef struct LoopContext {
	size_t depth;
	pos_stack* breaks;
	size_t start;
	struct LoopContext* next;
} LoopContext;

static LoopContext* loop_ctx;
static size_t depth;

static inline void compiler_init(void) {
	loop_ctx = NULL;
	depth = 0;
}

static void push_loop(size_t start) {
	LoopContext* ctx = gc_malloc(sizeof(LoopContext));
	ctx->depth = depth;
	ctx->breaks = pos_stack_create();
	ctx->start = start;
	ctx->next = loop_ctx;
	loop_ctx = ctx;
}

static void pop_loop(size_t end) {
	LoopContext* ctx = loop_ctx;
	loop_ctx = loop_ctx->next;

	while (!pos_stack_empty(ctx->breaks)) {
		patch_addr(pos_stack_pop(ctx->breaks), end);
	}

	pos_stack_free(ctx->breaks);
	gc_free(ctx);
}

enum {
	OP_NUM, OP_STR, OP_VAR, OP_TRUE, OP_FALSE, OP_NIL,
	OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD,
	OP_GT, OP_LT, OP_GE, OP_LE,
	OP_EQ, OP_NE,
	OP_POS, OP_NEG, OP_NOT,
	OP_POP, OP_JUMPT, OP_JUMPF, OP_JUMP,
	OP_STORE,
	OP_PUSH_CLOSURE, OP_POP_CLOSURE,
	OP_JUMPP,
	OP_FUNCDEF, OP_CALL, OP_TUPLE,
	OP_TABLE, OP_SETINDEX, OP_GETINDEX,
	OP_MULTIASSIGN,
	OP_ITER, OP_NEXT,
	OP_LIST,
	OP_HALT,

	#if TUG_DEBUG

	OP_DEBUG_PRINT,

	#endif
};

#if TUG_DEBUG

const char* get_opname(uint8_t op) {
	switch (op) {
		case OP_NUM: return "OP_NUM";
		case OP_STR: return "OP_STR";
		case OP_VAR: return "OP_VAR";
		case OP_TRUE: return "OP_TRUE";
		case OP_FALSE: return "OP_FALSE";
		case OP_NIL: return "OP_NIL";
		case OP_ADD: return "OP_ADD";
		case OP_SUB: return "OP_SUB";
		case OP_MUL: return "OP_MUL";
		case OP_DIV: return "OP_DIV";
		case OP_MOD: return "OP_MOD";
		case OP_GT: return "OP_GT";
		case OP_LT: return "OP_LT";
		case OP_GE: return "OP_GE";
		case OP_LE: return "OP_LE";
		case OP_EQ: return "OP_EQ";
		case OP_NE: return "OP_NE";
		case OP_POS: return "OP_POS";
		case OP_NEG: return "OP_NEG";
		case OP_NOT: return "OP_NOT";
		case OP_POP: return "OP_POP";
		case OP_JUMPT: return "OP_JUMPT";
		case OP_JUMPF: return "OP_JUMPF";
		case OP_JUMP: return "OP_JUMP";
		case OP_STORE: return "OP_STORE";
		case OP_PUSH_CLOSURE: return "OP_PUSH_CLOSURE";
		case OP_POP_CLOSURE: return "OP_POP_CLOSURE";
		case OP_JUMPP: return "OP_JUMPP";
		case OP_FUNCDEF: return "OP_FUNCDEF";
		case OP_CALL: return "OP_CALL";
		case OP_TUPLE: return "OP_TUPLE";
		case OP_TABLE: return "OP_TABLE";
		case OP_SETINDEX: return "OP_SETINDEX";
		case OP_GETINDEX: return "OP_GETINDEX";
		case OP_MULTIASSIGN: return "OP_MULTIASSIGN";
		case OP_ITER: return "OP_ITER";
		case OP_NEXT: return "OP_NEXT";
		case OP_HALT: return "OP_HALT";
		case OP_LIST: return "OP_LIST";
	
		#if TUG_DEBUG
	
		case OP_DEBUG_PRINT: return "OP_DEBUG_PRINT";
	
		#endif
		
		default: return NULL;
	};
}

#endif

static void emit_closure(int i) {
	if (i) {
		depth++;
		emit_byte(OP_PUSH_CLOSURE);
	} else {
		depth--;
		emit_byte(OP_POP_CLOSURE);
	}
}

static void compile_node(Node* node);
static void compile_block(NodeBlock* block) {
	for (size_t i = 0; i < block->count; i++) {
		Node* node = block->nodes[i];
		compile_node(node);

		if (node_isexpr(node)) {
			emit_byte(OP_POP);
			emit_addr(1);
		}
	}
}

static void compile_node(Node* node) {
	switch (node->kind) {
		case NUM: {
			emit_byte(OP_NUM);

			Node_Num* num = (Node_Num*)node->data;
			emit_num(num->num);
		} break;
		case STR:
		case NAME: {
			emit_byte(node->kind == STR ? OP_STR : OP_VAR);

			Node_Str* str = (Node_Str*)node->data;
			emit_str((const char*)str->str);
		} break;
		case TRUE: emit_byte(OP_TRUE); break;
		case FALSE: emit_byte(OP_FALSE); break;
		case NIL: emit_byte(OP_NIL); break;
		case ADD:
		case SUB:
		case MUL:
		case DIV:
		case MOD:
		case GT:
		case LT:
		case GE:
		case LE:
		case EQ:
		case NE:
		case INDEX: {
			Node_BinOp* binop = (Node_BinOp*)node->data;
			compile_node(binop->o1);
			compile_node(binop->o2);

			uint8_t op_emit;
			switch (node->kind) {
				case ADD: op_emit = OP_ADD; break;
				case SUB: op_emit = OP_SUB; break;
				case MUL: op_emit = OP_MUL; break;
				case DIV: op_emit = OP_DIV; break;
				case MOD: op_emit = OP_MOD; break;
				case GT: op_emit = OP_GT; break;
				case LT: op_emit = OP_LT; break;
				case GE: op_emit = OP_GE; break;
				case LE: op_emit = OP_LE; break;
				case EQ: op_emit = OP_EQ; break;
				case NE: op_emit = OP_NE; break;
				case INDEX: op_emit = OP_GETINDEX; break;
			}
			emit_byte(op_emit);
			if (node->kind != EQ && node->kind != NE) {
				emit_addr(binop->ln);
			}
		} break;

		#if TUG_DEBUG

		case DEBUG_PRINT: {
			Node_DebugPrint* debugprint = (Node_DebugPrint*)node->data;
			compile_node(debugprint->expr);

			emit_byte(OP_DEBUG_PRINT);
		} break;

		#endif

		case AND: {
			Node_BinOp* binop = (Node_BinOp*)node->data;
			compile_node(binop->o1);

			emit_byte(OP_JUMPF);
			size_t pos = emit_addr(0);
			emit_byte(1);

			emit_byte(OP_POP);
			emit_addr(1);
			compile_node(binop->o2);
			patch_addr(pos, main_bc->size);
		} break;

		case OR: {
			Node_BinOp* binop = (Node_BinOp*)node->data;
			compile_node(binop->o1);

			emit_byte(OP_JUMPT);
			size_t pos = emit_addr(0);
			emit_byte(1);

			emit_byte(OP_POP);
			emit_addr(1);
			compile_node(binop->o2);
			patch_addr(pos, main_bc->size);
		} break;
		
		case POS:
		case NEG:
		case NOT: {
			Node_Unary* unary = (Node_Unary*)node->data;
			compile_node(unary->right);
			emit_byte(node->kind == POS ? OP_POS : node->kind == NEG ? OP_NEG : OP_NOT);
			if (node->kind != NOT) {
				emit_addr(unary->ln);
			}
		} break;

		case IF: {
			Node_If* nif = (Node_If*)node->data;
			pos_stack* stack = pos_stack_create();

			compile_node(nif->cond);
			size_t upos = emit_jump(OP_JUMPF, 0, 0);
			emit_closure(1);
			compile_block(nif->block);
			emit_closure(0);
			emit_byte(OP_JUMP);
			pos_stack_push(stack, emit_addr(0));
			patch_addr(upos, main_bc->size);

			for (size_t i = 0; i < vec_count(nif->conds); i++) {
				Node* cond = vec_get(nif->conds, i);
				NodeBlock* block = vec_get(nif->blocks, i);

				compile_node(cond);
				upos = emit_jump(OP_JUMPF, 0, 0);
				emit_closure(1);
				compile_block(block);
				emit_closure(0);
				emit_byte(OP_JUMP);
				pos_stack_push(stack, emit_addr(0));
				patch_addr(upos, main_bc->size);
			}
			if (nif->eblock) {
				emit_closure(1);
				compile_block(nif->eblock);
				emit_closure(0);
			}

			while (!pos_stack_empty(stack)) {
				patch_addr(pos_stack_pop(stack), main_bc->size);
			}

			pos_stack_free(stack);
		} break;

		case WHILE: {
			Node_While* nwhile = (Node_While*)node->data;

			push_loop(main_bc->size);
			compile_node(nwhile->cond);
			size_t upos = emit_jump(OP_JUMPF, 0, 0);

			emit_closure(1);
			compile_block(nwhile->block);
			emit_closure(0);

			emit_byte(OP_JUMP);
			emit_addr(loop_ctx->start);

			patch_addr(upos, main_bc->size);
			pop_loop(main_bc->size);
		} break;

		case BREAK: {
			emit_byte(OP_JUMPP);
			emit_addr(depth - loop_ctx->depth);
			pos_stack_push(loop_ctx->breaks, emit_addr(0));
		} break;

		case CONTINUE: {
			emit_byte(OP_JUMPP);
			emit_addr(depth - loop_ctx->depth - 1);
			emit_addr(loop_ctx->start);
		} break;

		case FUNCDEF: {
			Node_FuncDef* funcdef = (Node_FuncDef*)node->data;

			emit_byte(OP_FUNCDEF);
			emit_addr(funcdef->ln);
			if (funcdef->names == NULL) {
				emit_addr(0);
			} else {
				emit_addr(vec_count(funcdef->names));
				for (size_t i = 0; i < vec_count(funcdef->names); i++) {
					emit_str((const char*)vec_get(funcdef->names, i));
				}
			}

			Vector* params = funcdef->params;
			emit_addr(vec_count(params));
			vec_iter(params, emit_str);

			Bytecode* prev = main_bc;
			main_bc = bc_create();
			compile_block(funcdef->block);
			emit_byte(OP_NIL);
			emit_byte(OP_HALT);
			Bytecode* temp = main_bc;
			main_bc = prev;

			emit_bc(temp);
			bc_free(temp);

			if (funcdef->names != NULL && vec_count(funcdef->names) == 1) {
				emit_byte(OP_STORE);
				emit_byte(1);
				emit_addr(1);
				emit_str(vec_get(funcdef->names, 0));
			}
		} break;

		case FUNCCALL: {
			Node_FuncCall* funccall = (Node_FuncCall*)node->data;
			compile_node(funccall->node);
			vec_iter(funccall->values, compile_node);

			emit_byte(OP_CALL);
			emit_addr(vec_count(funccall->values));
			emit_addr(funccall->ln);
		} break;

		case RETURN: {
			if (node->data == NULL) {
				emit_byte(OP_NIL);
			} else {
				Vector* values = (Vector*)node->data;
				size_t val_count = vec_count(values);

				if (val_count == 1) {
					compile_node(vec_get(values, 0));
				} else {
					for (size_t i = 0; i < val_count; i++) {
						compile_node(vec_get(values, i));
					}
					emit_byte(OP_TUPLE);
					emit_addr(val_count);
				}
			}
			emit_byte(OP_HALT);
		} break;

		case TABLE: {
			emit_byte(OP_TABLE);
			if (!node->data) break;

			Node_Table* ntable = (Node_Table*)node->data;
			for (size_t i = 0; i < vec_count(ntable->keys); i++) {
				Node* key = vec_get(ntable->keys, i);
				Node* value = vec_get(ntable->values, i);

				if (key) {
					compile_node(key);
					compile_node(value);
					emit_byte(OP_SETINDEX);
					emit_addr(0);
					emit_byte(1);
				} else {
					emit_byte(OP_NUM);
					emit_num((double)i);
					compile_node(value);
					emit_byte(OP_SETINDEX);
					emit_addr(0);
					emit_byte(1);
				}
			}
		} break;
		
		case ASSIGN: {
			Node_Assignment* assignment = (Node_Assignment*)node->data;
			Vector* assigns = assignment->assigns;

			for (size_t i = 0; i < vec_count(assigns); i++) {
				Assign* assign = vec_get(assigns, i);

				if (assign->kind == SETINDEX) {
					compile_node(assign->obj);
					compile_node(assign->key);
				}
			}

			vec_iter(assignment->values, compile_node);

			emit_byte(OP_MULTIASSIGN);
			emit_addr(assignment->ln);
			emit_byte(assignment->local);
			emit_addr(vec_count(assignment->values));
			emit_addr(vec_count(assigns));
			for (size_t i = vec_count(assigns); i-- > 0;) {
				Assign* assign = vec_get(assigns, i);
				if (assign->kind == ASSIGN) {
					emit_byte(1);
					emit_str(assign->name);
				} else {
					emit_byte(0);
				}
			}
		} break;

		case FOR: {
			Node_For* nfor = (Node_For*)node->data;

			emit_closure(1);

			compile_node(nfor->node);
			emit_byte(OP_ITER);
			emit_addr(nfor->ln);

			push_loop(main_bc->size);
			emit_byte(OP_NEXT);
			emit_addr(nfor->ln);
			Vector* names = nfor->names;
			emit_addr(vec_count(names));
			vec_iter(names, emit_str);
			size_t upos = emit_addr(0);

			compile_block(nfor->block);

			emit_byte(OP_JUMP);
			emit_addr(loop_ctx->start);

			patch_addr(upos, main_bc->size);
			pop_loop(main_bc->size);

			emit_closure(0);
		} break;
		
		case LIST: {
			Vector* list = (Vector*)node->data;
			if (list) {
				size_t count = vec_count(list);
				for (size_t i = 0; i < count; i++) {
					compile_node(vec_get(list, i));
				}
				
				emit_byte(OP_LIST);
				emit_addr(count);
			} else {
				emit_byte(OP_LIST);
				emit_addr(0);
			}
		} break;
	}
}

#if TUG_DEBUG

typedef struct {
	Bytecode* bc;
	size_t ptr;
	size_t scope;
} BCReader;

void bcreader_init(BCReader* reader, Bytecode* bc, size_t scope) {
	reader->bc = bc;
	reader->ptr = 0;
	reader->scope = 0;
}

uint8_t bcreader_byte(BCReader* reader) {
	return reader->bc->data[reader->ptr++];
}

double bcreader_num(BCReader* reader) {
	double num;
	memcpy(&num, &reader->bc->data[reader->ptr], sizeof(double));
	reader->ptr += sizeof(double);
	return num;
}

const char* bcreader_str(BCReader* reader) {
	const char* str = (const char*)&reader->bc->data[reader->ptr];
	size_t len = strlen(str) + 1;
	reader->ptr += len;
	return str;
}

size_t bcreader_addr(BCReader* reader) {
	size_t addr;
	memcpy(&addr, &reader->bc->data[reader->ptr], sizeof(size_t));
	reader->ptr += sizeof(size_t);
	return addr;
}

int bcreader_read(BCReader* reader);
void bcreader_bc(BCReader* reader) {
	size_t size = bcreader_addr(reader);
	uint8_t* data = gc_malloc(size);
	memcpy(data, &reader->bc->data[reader->ptr], size);
	reader->ptr += size;

	Bytecode* bc = gc_malloc(sizeof(Bytecode));
	bc->size = bc->capacity = size;
	bc->data = data;
	bc->ref = 0;

	BCReader R;
	bcreader_init(&R, bc, reader->scope + 1);
	while (bcreader_read(&R));
	gc_free(data);
	gc_free(bc);
}

int bcreader_read(BCReader* reader) {
	if (reader->ptr >= reader->bc->size) return 0;

	for (size_t i = 0; i < reader->scope; i++) {
		printf("  ");
	}
	printf("%zu ", reader->ptr);
	uint8_t op = bcreader_byte(reader);
	const char* opname = get_opname(op);
	printf("%s ", opname);
	switch (op) {
		case OP_NUM: {
			double num = bcreader_num(reader);
			printf("%.17g", num);
		} break;
		case OP_STR: 
		case OP_VAR: {
			const char* str = bcreader_str(reader);
			printf("|%s|", str);
		} break;
		case OP_ADD:
		case OP_SUB:
		case OP_MUL:
		case OP_DIV:
		case OP_MOD: 
		case OP_GT:
		case OP_LT: 
		case OP_GE:
		case OP_LE: 
		case OP_EQ:
		case OP_NE:
		case OP_POS:
		case OP_NEG:
		case OP_NOT:
		case OP_GETINDEX:
		case OP_ITER: {
			size_t ln = bcreader_addr(reader);
			printf("ln:%zu", ln);
		} break;

		case OP_HALT:
		case OP_TRUE:
		case OP_FALSE:
		case OP_NIL:
		case OP_DEBUG_PRINT:
		case OP_PUSH_CLOSURE:
		case OP_POP_CLOSURE:
		case OP_TABLE:
		break;

		case OP_POP:
		case OP_JUMP:
		case OP_TUPLE:
		case OP_LIST: {
			size_t c = bcreader_addr(reader);
			printf("%zu", c);
		} break;

		case OP_JUMPT:
		case OP_JUMPF: {
			size_t addr = bcreader_addr(reader);
			uint8_t pback = bcreader_byte(reader);
			printf("addr:%zu pback:%d", addr, pback);
		} break;

		case OP_STORE: {
			uint8_t local = bcreader_byte(reader);
			size_t count = bcreader_addr(reader);
			printf("local:%d count:%zu", local, count);
			for (size_t i = 0; i < count; i++) {
				const char* name = bcreader_str(reader);
				printf(" %s", name);
			}
		} break;

		case OP_JUMPP: {
			printf("count:%zu addr:%zu", bcreader_addr(reader), bcreader_addr(reader));
		} break;

		case OP_FUNCDEF: {
			size_t ln = bcreader_addr(reader);
			size_t namec = bcreader_addr(reader);
			printf("ln:%zu namec:%zu", ln, namec);
			if (namec == 0) {
				printf(" <anonymous>");
			} else {
				for (size_t i = 0; i < namec; i++) {
					printf(" %s", bcreader_str(reader));
				}
			}

			size_t count = bcreader_addr(reader);
			printf(" count:%zu\n", count);
			bcreader_bc(reader);
		} break;

		case OP_CALL: {
			size_t argc = bcreader_addr(reader);
			size_t ln = bcreader_addr(reader);
			printf("argc:%zu ln:%zu", argc, ln);
		} break;

		case OP_SETINDEX: {
			size_t ln = bcreader_addr(reader);
			uint8_t push = bcreader_byte(reader);
			printf("ln:%zu push:%d", ln, push);
		} break;

		case OP_MULTIASSIGN: {
			size_t ln = bcreader_addr(reader);
			uint8_t local = bcreader_byte(reader);
			size_t valuec = bcreader_addr(reader);
			size_t assignc = bcreader_addr(reader);

			printf("ln:%zu local:%d valuec:%zu assignc:%zu", ln, local, valuec, assignc);
			printf(" kinds:");
			for (size_t i = 0; i < assignc; i++) {
				uint8_t kind = bcreader_byte(reader);
				printf("%d", kind);
				if (kind) {
					const char* name = bcreader_str(reader);
					printf(":%s", name);
				}
				if (i < assignc - 1) printf(",");
			}
		} break;
		
		case OP_NEXT: {
			size_t ln = bcreader_addr(reader);
			size_t count = bcreader_addr(reader);
			printf("ln:%zu count:%zu", ln, count);

			for (size_t i = 0; i < count; i++) {
				const char* name = bcreader_str(reader);
				printf(" %s", name);
			}

			size_t pos = bcreader_addr(reader);
			printf(" pos:%zu", pos);
		} break;
	}

	printf("\n");
	return 1;
}

#endif

static Bytecode* gen_bc(const char* src, const char* text, char* errmsg) {
	pinit(src, text);
	if (ltok()) {
		pprint_err(errmsg);
		pfree();
		return NULL;
	}

	Bytecode* bc = bc_create();
	main_bc = bc;
	while (tkind != EOF) {
		if (pstmt()) {
			pprint_err(errmsg);
			pfree();
			bc_free(bc);
			return NULL;
		}

		compile_node(node);
		if (node_isexpr(node)) {
			emit_byte(OP_POP);
			emit_addr(1);
		}
		node_free(node);
		node = NULL;
	}
	emit_byte(OP_HALT);

	#if TUG_DEBUG

	BCReader R;
	bcreader_init(&R, bc, 0);
	while (bcreader_read(&R));
	printf("\n");

	#endif

	return bc;
}

struct VarMap;
struct Table;
struct TableEntry;
typedef struct tug_Object {
	int kind;
	union {
		struct {
			char* str;
			uint8_t m;
		};
		double num;
		struct {
			char* src;
			char* name;
			Vector* params;
			Bytecode* bc;
			struct VarMap* upper;
			tug_CFunc cfunc;
		} func;
		Vector* tuple;
		struct {
			struct Table* table;
			struct tug_Object* metatable;
		};
		struct {
			struct tug_Object* obj;
			size_t len;
			size_t idx;
			struct TableEntry* entry;
		} iter;
		Vector* list;
	};
	uint8_t collected;
	uint8_t marked;
	uint64_t id;
} Object;

static Object __obj_true = {TRUE, NULL};
static Object __obj_false = {FALSE, NULL};
static Object __obj_nil = {NIL, NULL};

#define obj_true (&__obj_true)
#define obj_false (&__obj_false)
#define obj_nil (&__obj_nil)
#define obj_truth(c) ((c) ? obj_true : obj_false)

static uint64_t seed_id = 0;
static size_t next_id = 0;

static Object* obj_pool[OBJ_POOL_LIMIT];
static size_t obj_poolc = 0;

static Object* obj_create(int kind) {
	Object* obj;
	if (obj_poolc > 0) {
		obj = obj_pool[--obj_poolc];
	} else {
		obj = gc_malloc(sizeof(Object));
	}
	obj->kind = kind;
	obj->str = NULL;
	obj->m = 0;
	obj->marked = 0;
	obj->collected = 0;

	obj->id = (next_id++ ^ (seed_id & 0xFFFFFF));

	return obj;
}

static Object* obj_iter(Object* obj) {
	int kind = -1;
	if (obj->kind == TABLE) {
		Object* meta = tuglib_getmetafield(obj, "__next");
		if (meta != obj_nil) return obj;
		kind = ITER_TABLE;
	}
	else if (obj->kind == STR) kind = ITER_STR;
	else if (obj->kind == LIST) kind = ITER_LIST;
	else return NULL;

	Object* iter_obj = obj_create(kind);
	if (obj->kind == STR) iter_obj->iter.len = strlen(obj->str);
	else iter_obj->iter.len = 0;
	iter_obj->iter.idx = 0;
	iter_obj->iter.entry = NULL;
	iter_obj->iter.obj = obj;

	return iter_obj;
}

static Object* obj_str(char* str) {
	Object* obj = obj_create(STR);
	obj->str = str;

	return obj;
}

static Object* obj_num(double num) {
	Object* obj = obj_create(NUM);
	obj->num = num;

	return obj;
}

// Name will not be duplicated
// Bytecode ref-count will not be increased
// Params will not also be duplicated
static Object* obj_func(char* src, char* name, Vector* params, Bytecode* bc, struct VarMap* upper) {
	Object* obj = obj_create(FUNC);
	obj->func.src = src;
	obj->func.name = name;
	obj->func.params = params;
	obj->func.bc = bc;
	obj->func.upper = upper;
	obj->func.cfunc = NULL;

	return obj;
}

// Name will be duplicated
static Object* obj_cfunc(const char* name, tug_CFunc cfunc) {
	Object* obj = obj_create(FUNC);
	obj->func.src = "[C]";
	obj->func.name = gc_strdup(name);
	obj->func.params = NULL;
	obj->func.bc = NULL;
	obj->func.upper = NULL;
	obj->func.cfunc = cfunc;

	return obj;
}

static struct Table* table_create();
static Object* obj_table(struct Table* table) {
	if (!table) {
		table = table_create();
	}

	Object* obj = obj_create(TABLE);
	obj->table = table;
	obj->metatable = obj_nil;

	return obj;
}

static void table_free(struct Table* table);
static void obj_free(Object* obj) {
	switch (obj->kind) {
		case STR: {
			if (obj->m) free(obj->str);
			else gc_free(obj->str);	
		} break;
		case FUNC: {
			if (!obj->func.cfunc) {
				gc_free(obj->func.src);
				vec_stdfree(obj->func.params);
				bc_free(obj->func.bc);
			}
			gc_free(obj->func.name);
		} break;
		case LIST: vec_free(obj->list); break;
		case TUPLE: vec_free(obj->tuple); break;
		case TABLE: table_free(obj->table); break;
	}
	
	if (obj_poolc < OBJ_POOL_LIMIT) {
		obj_pool[obj_poolc++] = obj;
	} else gc_free(obj);
}

static const char* obj_type(Object* obj) {
	switch (obj->kind) {
		case STR: return "str";
		case NUM: return "num";
		case TRUE:
		case FALSE: return "bool";
		case NIL: return "nil";
		case FUNC: return "func";
		case TABLE: return "table";
		case LIST: return "list";
		default: return "unknown";
	}
}

#if TUG_DEBUG
static void obj_print(Object* obj) {
	switch (obj->kind) {
		case STR: {
			printf("%s\n", obj->str);
		} break;
		case NUM: {
			printf("%.17g\n", obj->num);
		} break;
		case TRUE: {
			printf("true\n");
		} break;
		case FALSE: {
			printf("false\n");
		} break;
		case NIL: {
			printf("nil\n");
		} break;
		case FUNC: {
			printf("func: 0x%lx\n", obj->id);
		} break;
		case TUPLE: {
			obj_print(vec_get(obj->tuple, vec_count(obj->tuple) - 1));
		} break;
		case TABLE: {
			printf("table: 0x%lx\n", obj->id);
		} break;
		case LIST: {
			printf("list: 0x%lx\n", obj->id);
		} break;
		default: {
			printf("unknown\n");
		} break;
	}
}
#endif

static int obj_compare(Object* o1, Object* o2) {
	if (o1->kind != o2->kind) return 0;
	switch (o1->kind) {
		case NUM: return o1->num == o2->num;
		case STR: return strcmp(o1->str, o2->str) == 0;
		case TRUE:
		case FALSE:
		case NIL: return 1;
		default: return o1 == o2;
	}
}

static int obj_check(Object* obj) {
	switch (obj->kind) {
		case NUM: return obj->num != 0;
		case STR: return obj->str[0] != '\0';
		case FALSE:
		case NIL: return 0;
		case LIST: return obj->list->count != 0;
		default: return 1;
	}
}

static uint64_t obj_hash(Object* obj) {
	switch (obj->kind) {
		case NUM: {
			union { double d; uint64_t u; } u;
			u.d = obj->num;
			if (u.u == 0x8000000000000000ULL) u.u = 0;

			return u.u * 11400714819323198485ULL;
		} break;
		case STR: {
			uint64_t hash = 1469598103934665603ULL;
			char* ptr = obj->str;
			while (*ptr) {
				hash ^= (unsigned char)(*ptr++);
				hash *= 1099511628211ULL;
			}

			return hash;
		} break;
		case TRUE: return 1231;
		case FALSE: return 1237;
		default: {
			uintptr_t hash = (uintptr_t)obj;

			hash ^= hash >> 33;
			hash *= 0xff51afd7ed558ccdULL;
			hash ^= hash >> 33;
			hash *= 0xc4ceb9fe1a85ec53ULL;
			hash ^= hash >> 33;

			return (uint64_t)hash;
		} break;
	}
}

typedef struct TableEntry {
	Object* key;
	Object* value;
	struct TableEntry* next;
} TableEntry;

typedef struct Table {
	TableEntry** buckets;
	size_t capacity;
	size_t count;
} Table;

static struct Table* table_create() {
	struct Table* table = gc_malloc(sizeof(struct Table));
	table->count = 0;
	table->capacity = 0;
	table->buckets = NULL;

	return table;
}

static void table_resize(Table* table, size_t new_cap) {
	TableEntry** new_buckets = gc_calloc(new_cap, sizeof(TableEntry*));

	for (size_t i = 0; i < table->capacity; i++) {
		TableEntry* entry = table->buckets[i];

		while (entry) {
			TableEntry* next = entry->next;
			uint64_t idx = obj_hash(entry->key) & (new_cap - 1);

			entry->next = new_buckets[idx];
			new_buckets[idx] = entry;

			entry = next;
		}
	}

	gc_free(table->buckets);
	table->buckets = new_buckets;
	table->capacity = new_cap;
}

// smart resize i think
static void table_smresize(Table* table) {
	if (table->capacity == 0) {
		table_resize(table, 8);
		return;
	}

	double lf = (double)table->count / table->capacity;

	if (lf > 0.8) {
		table_resize(table, table->capacity * 2);
	} else if (lf < 0.2 && table->capacity > 8) {
		size_t new_cap = table->capacity / 2;
		if (new_cap < 8) new_cap = 8;
		table_resize(table, new_cap);
	}
}

static void table_remove(Table* table, Object* key) {
	size_t index = obj_hash(key) & (table->capacity - 1);

	TableEntry* prev = NULL;
	for (TableEntry* entry = table->buckets[index]; entry; prev = entry, entry = entry->next) {
		if (obj_compare(entry->key, key)) {
			if (prev) prev->next = entry->next;
			else table->buckets[index] = entry->next;

			gc_free(entry);
			table->count--;
			break;
		}
	}

	table_smresize(table);
}

static Object* table_get(Table* table, Object* key) {
	if (table->buckets == NULL) return obj_nil;

	size_t index = obj_hash(key) & (table->capacity - 1);

	for (TableEntry* entry = table->buckets[index]; entry; entry = entry->next) {
		if (obj_compare(entry->key, key)) return entry->value;
	}

	return obj_nil;
}

static void table_set(Table* table, Object* key, Object* value) {
	if (value == obj_nil) {
		table_remove(table, key);
		return;
	}

	table_smresize(table);

	size_t index = obj_hash(key) & (table->capacity - 1);
	if (table->buckets) {
		for (TableEntry* entry = table->buckets[index]; entry; entry = entry->next) {
			if (obj_compare(entry->key, key)) {
				entry->value = value;
				return;
			}
		}
	}

	TableEntry* new_entry = gc_malloc(sizeof(TableEntry));
	new_entry->key = key;
	new_entry->value = value;
	new_entry->next = table->buckets[index];
	table->buckets[index] = new_entry;
	table->count++;
}

static void table_free(struct Table* table) {
	if (!table || !table->buckets) {
		gc_free(table);
		return;
    }

    for (size_t i = 0; i < table->capacity; i++) {
	TableEntry* entry = table->buckets[i];
	while (entry) {
	    TableEntry* next = entry->next;
	    gc_free(entry);
	    entry = next;
	}
    }

    gc_free(table->buckets);
    gc_free(table);
}

typedef struct VarMapEntry {
	char* key;
	Object* value;
	struct VarMapEntry* next;
} VarMapEntry;

typedef struct VarMap {
	VarMapEntry** buckets;
	size_t capacity;
	size_t count;
	int marked;
	struct VarMap* next;
} VarMap;

static uint64_t hash_str(const char* str) {
	uint64_t hash = 5381;
	int c;
	while ((c = *str++)) hash = ((hash << 5) + hash) + c;

	return hash;
}

static VarMap* varmap_pool[VARMAP_POOL_LIMIT];
static size_t varmap_poolc = 0;
static VarMapEntry* varmapentry_pool[VARMAPENTRY_POOL_LIMIT];
static size_t varmapentry_poolc = 0;

static VarMapEntry* varmapentry_create(const char* key, Object* value) {
	VarMapEntry* entry;
	if (varmapentry_poolc > 0) {
		entry = varmapentry_pool[--varmapentry_poolc];
	} else entry = gc_malloc(sizeof(VarMapEntry));
	
	entry->key = gc_strdup(key);
	entry->value = value;
	return entry;
}

static void varmapentry_free(VarMapEntry* entry) {
	gc_free(entry->key);
	if (varmapentry_poolc < VARMAPENTRY_POOL_LIMIT) {
		varmapentry_pool[varmapentry_poolc++] = entry;
	} else {
		gc_free(entry);
	}
}

static VarMap* varmap_create() {
	VarMap* map;
	if (varmap_poolc > 0) {
		map = varmap_pool[--varmap_poolc];
	} else {
		map = gc_malloc(sizeof(VarMap));
		map->buckets = gc_calloc(8, sizeof(VarMapEntry*));
	}
	
	map->capacity = 8;
	map->count = 0;
	map->marked = 0;
	map->next = NULL;
	
	memset(map->buckets, 0, map->capacity * sizeof(VarMapEntry*));

	return map;
}

static void varmap_resize(VarMap* map);
static void varmap_put(VarMap* map, const char* key, Object* value) {
	uint64_t index = hash_str(key) % map->capacity;
	VarMapEntry* entry = map->buckets[index];

	while (entry) {
		if (streq(entry->key, key)) {
			entry->value = value;
			return;
		}

		entry = entry->next;
	}

	entry = varmapentry_create(key, value);
	entry->next = map->buckets[index];
	map->buckets[index] = entry;
	map->count++;

	if ((double)map->count / map->capacity > 0.75) {
		varmap_resize(map);
	}
}

static void varmap_edit(VarMap* map, const char* key, Object* value) {
	VarMap* prev = NULL;
	while (map) {
		uint64_t index = hash_str(key) % map->capacity;
		VarMapEntry* entry = map->buckets[index];

		while (entry) {
			if (streq(entry->key, key)) {
				entry->value = value;
				return;
			}

			entry = entry->next;
		}

		prev = map;
		map = map->next;
	}

	if (prev) {
		varmap_put(prev, key, value);
	}
}

static void varmap_resize(VarMap* map) {
	size_t new_capacity = map->capacity * 2;
	VarMapEntry** new_buckets = gc_calloc(new_capacity, sizeof(VarMapEntry*));

	for (size_t i = 0; i < map->capacity; i++) {
		VarMapEntry* entry = map->buckets[i];
		while (entry) {
			VarMapEntry* next = entry->next;
			uint64_t index = hash_str(entry->key) % new_capacity;
			entry->next = new_buckets[index];
			new_buckets[index] = entry;

			entry = next;
		}
	}

	gc_free(map->buckets);
	map->buckets = new_buckets;
	map->capacity = new_capacity;
}

static Object* __varmap_get(VarMap* map, const char* key, uint8_t* found) {
	if (found) *found = 0;
	while (map) {
		uint64_t index = hash_str(key) % map->capacity;
		VarMapEntry* entry = map->buckets[index];

		while (entry) {
			if (streq(entry->key, key)) {
				if (found) *found = 1;
				return entry->value;
			}
			entry = entry->next;
		}

		map = map->next;
	}

	return obj_nil;
}

#define varmap_get(M, k) __varmap_get((M), (k), NULL)

static void varmap_free(VarMap* map) {
	for (size_t i = 0; i < map->capacity; i++) {
		VarMapEntry* entry = map->buckets[i];

		while (entry) {
			VarMapEntry* next = entry->next;
			varmapentry_free(entry);
			entry = next;
		}
	}
	
	if (varmap_poolc < VARMAP_POOL_LIMIT) {
		varmap_pool[varmap_poolc++] = map;
	} else {
		gc_free(map->buckets);
		gc_free(map);
	}
}

typedef struct Info {
	char* src;
	char* name;
	size_t ln;
	struct Info* next;
} Info;

static inline void info_free(Info* info) {
	while (info) {
		gc_free(info->src);
		gc_free(info->name);
		Info* next = info->next;
		gc_free(info);
		info = next;
	}
}

typedef struct Frame {
	char* src;
	char* name;
	size_t ln;
	Bytecode* bc;
	size_t iptr;
	size_t scope;
	size_t base;
	Vector* args;
	Object* ret;
	int protected;
	struct Frame* next;
} Frame;

static Frame* frame_create(const char* src, const char* name, Bytecode* bc, size_t scope, size_t base, Vector* args) {
	Frame* frame = gc_malloc(sizeof(Frame));
	frame->src = gc_strdup(src);
	frame->name = gc_strdup(name);
	frame->bc = bc;
	if (bc) bc->ref++;
	frame->iptr = 0;
	frame->scope = scope;
	frame->base = base;
	frame->args = args;
	frame->ret = obj_nil;
	frame->protected = 0;
	frame->next = NULL;

	return frame;
}

static void frame_free(Frame* frame, Info** info_p) {
	if (!frame) return;
	if (info_p) {
		Info* info = gc_malloc(sizeof(Info));
		info->src = frame->src;
		info->name = frame->name;
		info->ln = frame->ln;
		info->next = (*info_p);
		(*info_p) = info;
	} else {
		gc_free(frame->src);
		gc_free(frame->name);
	}
	bc_free(frame->bc);
	vec_free(frame->args);
	gc_free(frame);
}

enum {
	TASK_NEW,
	TASK_YIELD,
	TASK_RUNNING,
	TASK_ERROR,
	TASK_END,
};

typedef struct tug_Task {
	Frame* frame;
	Vector* varmaps;
	VarMap* global;
	size_t frame_count;
	Vector* stack;
	Info* info;
	char msg[2048];
	int state;
} Task;

static void gc_collect_closure(VarMap* varmap);
static void gc_collect_task(Task* task);
static Task* task_create(const char* src, Bytecode* bc) {
	Task* task = gc_malloc(sizeof(Task));
	task->frame = frame_create(src, "<main>", bc, 0, 0, NULL);
	task->varmaps = vec_create();
	VarMap* map = varmap_create();
	vec_push(task->varmaps, map);
	task->global = varmap_create();
	task->frame_count = 1;
	task->stack = vec_create();
	task->info = NULL;
	task->state = TASK_NEW;

	gc_collect_closure(task->global);
	gc_collect_closure(map);
	gc_collect_task(task);

	return task;
}

#define get_base(T) ((!(T)->frame) ? 0 : (T)->frame->base)

static uint8_t read_byte(Task* task) {
	Frame* frame = task->frame;
	uint8_t value = frame->bc->data[frame->iptr];
	frame->iptr++;

	return value;
}

static double read_num(Task* task) {
	Frame* frame = task->frame;
	double value;
	memcpy(&value, &frame->bc->data[frame->iptr], sizeof(double));
	frame->iptr += sizeof(double);

	return value;
}

static const char* read_str(Task* task) {
	Frame* frame = task->frame;

	const char* str = (const char*)&frame->bc->data[frame->iptr];
	size_t len = strlen(str) + 1;
	frame->iptr += len;

	return str;
}

static size_t read_addr(Task* task) {
	Frame* frame = task->frame;
	size_t value;
	memcpy(&value, &frame->bc->data[frame->iptr], sizeof(size_t));
	frame->iptr += sizeof(size_t);

	return value;
}

static Bytecode* read_bc(Task* task) {
	Frame* frame = task->frame;
	Bytecode* bc = gc_malloc(sizeof(Bytecode));

	size_t size = read_addr(task);
	uint8_t* data = gc_malloc(size);
	memcpy(data, &frame->bc->data[frame->iptr], size);
	frame->iptr += size;

	bc->size = size;
	bc->capacity = size;
	bc->data = data;
	bc->ref = 0;

	return bc;
}

#define set_addr(__T, __addr) ((__T)->frame->iptr = (__addr))

static void push_obj(Task* task, Object* obj) {
	vec_push(task->stack, obj);
}

static void gc_collect_obj(Object* obj);
static inline Object* gc_obj(Object* obj) {
	gc_collect_obj(obj);

	return obj;
}

#define new_num(__num) gc_obj(obj_num(__num))
#define new_str(__str) gc_obj(obj_str((char*)__str))

static inline VarMap* get_map(Task* task);
// Expecting `params` must be an array of `const char*`
// `params` will be duplicated
// `src` will be duplicated
// `name` will not be duplicated
// `bc` will be increased (reference count)
static Object* new_func(Task* task, char* name, Vector* params, Bytecode* bc) {
	bc->ref++;
	size_t count = vec_count(params);
	Vector* dparams = vec_serve(count);
	for (size_t i = 0; i < count; i++) {
		vec_push(dparams, gc_strdup((char*)vec_get(params, i)));
	}
	char* nsrc = gc_strdup(task->frame->src);

	Object* obj = obj_func(nsrc, name, dparams, bc, get_map(task));
	gc_collect_obj(obj);

	return obj;
}

#define new_table() gc_obj(obj_table(NULL))

#define push_func(T, __name, __params, __bc) push_obj((T), new_func((T), (__name), (__params), (__bc)))
#define push_num(T, __num) push_obj((T), new_num((__num)))

// `__str` will not be duplicated
#define push_str(T, __str) push_obj((T), new_str((__str)))
#define push_newtable(T) push_obj((T), new_table())

static Vector* pop_nvalue(Task* task, size_t n) {
	Vector* res = vec_serve(n);
	Frame* frame = task->frame;
	Vector* stack = task->stack;

	for (size_t i = 0; i < n; i++) {
		Object* obj;

		if (get_base(task) >= stack->count) {
			obj = obj_nil;
		} else {
			obj = vec_pop(stack);
			obj = obj ? obj : obj_nil;
		}

		if (vec_count(res) < n) {
			if (obj->kind == TUPLE) {
				while (vec_count(res) < n && vec_count(obj->tuple) > 0) {
					vec_pushfirst(res, vec_pop(obj->tuple));
				}
			} else vec_pushfirst(res, obj);
		}
	}

	return res;
}

static Object* pop_value(Task* task) {
	Vector* objs = pop_nvalue(task, 1);
	Object* obj = vec_pop(objs);
	vec_free(objs);

	return obj;
}

static Object* pop_tvalue(Task* task) {
	Frame* frame = task->frame;
	Vector* stack = task->stack;
		
	if (get_base(task) >= stack->count) return obj_nil;

	return vec_pop(stack);
}

static Object* peek_tvalue(Task* task) {
	Frame* frame = task->frame;
	Vector* stack = task->stack;

	if (get_base(task) >= stack->count) return obj_nil;
	return vec_peek(stack);
}

static Object* peek_value(Task* task) {
	Object* obj = peek_tvalue(task);
	if (obj->kind == TUPLE) return vec_peek(obj->tuple);
	return obj;
}

static void assign_err(Task* task, const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);
	vsnprintf(task->msg, sizeof(task->msg), fmt, args);
	va_end(args);
	task->state = TASK_ERROR;
}

static inline VarMap* get_map(Task* task) {
	return vec_get(task->varmaps, vec_count(task->varmaps) - 1);
}

static Object* get_var(Task* task, const char* name) {
	uint8_t found;
	Object* res = __varmap_get(get_map(task), name, &found);

	if (!found) return varmap_get(task->global, name);
	return res;
}

// `__name` must be `const char*`
#define set_var(__T, __name, __val) varmap_put(get_map((__T)), (__name), (__val))

// `__name` must be `const char*`
#define edit_var(__T, __name, __val) varmap_edit(get_map((__T)), (__name), (__val))

static Object* get_arg(Task* task, size_t idx) {
	Frame* frame = task->frame;
	if (frame->args == NULL) return obj_nil;
	Object* res = vec_get(frame->args, idx);
	if (res == NULL) return obj_nil;

	return res;
}

#define get_argc(__T) (((__T)->frame->args == NULL) ? 0 : vec_count((__T)->frame->args))

jmp_buf cfunc_jmp_buf;

void call_obj(Task* task, Object* obj, Vector* args, int f, int protected) {
	if (obj->kind == TABLE && obj->metatable != obj_nil) {
		Table* mtable = obj->metatable->table;
		vec_pushfirst(args, obj);
		Object* func = table_get(mtable, tug_conststr("__call"));
		if (func != obj_nil) obj = func;
		if (obj->kind != FUNC) {
			if (f) vec_free(args);
			assign_err(task, "metamethod '__call' must be 'func', got '%s'", obj_type(obj));
			return;
		}
	}
	if (obj->kind != FUNC) {
		if (f) vec_free(args);
		assign_err(task, "unable to call '%s'", obj_type(obj));
		return;
	} else if (task->frame_count >= TUG_CALL_LIMIT) {
		if (f) vec_free(args);
		assign_err(task, "stack overflow");
		return;
	}

	Frame* new_frame = frame_create(obj->func.src, obj->func.name, obj->func.bc, vec_count(task->varmaps), vec_count(task->stack), args);
	new_frame->next = task->frame;
	task->frame->protected = protected;
	task->frame = new_frame;
	task->frame_count++;

	if (!obj->func.cfunc) {
		VarMap* func_map = obj->func.upper;
		VarMap* func_env = varmap_create();
		gc_collect_closure(func_env);
		func_env->next = func_map;
		vec_push(task->varmaps, func_env);
		size_t argc = vec_count(args);
		size_t paramc = vec_count(obj->func.params);
		for (size_t i = 0; i < paramc; i++) {
			if (i >= argc) set_var(task, vec_get(obj->func.params, i), obj_nil);
			else set_var(task, vec_get(obj->func.params, i), vec_get(args, i));
		}
	} else {
		if (setjmp(cfunc_jmp_buf) == 0) {
			obj->func.cfunc(task);
		}

		if (task->state == TASK_RUNNING) {
			push_obj(task, task->frame->ret);

			Frame* next_frame = task->frame->next;
			frame_free(task->frame, NULL);
			task->frame = next_frame;
			task->frame_count--;
		}
	}
}

static void set_ret(Task* task, Object* obj) {
	if (task->frame) task->frame->ret = obj;
}

static Object* get_ret(Task* task) {
	if (task->frame) return task->frame->ret;
	return obj_nil;
}

static void gc_run(void);
static void task_exec(Task* task) {
	#define call_fobj(_obj, _args) call_obj(task, (_obj), (_args), 1, 0); if (!tuglib_iserr(task)) {task_exec(task); if (tuglib_iserr(task)) break;}
	if (task->frame->bc == NULL) return;

	#if TUG_DEBUG
	uint8_t prev_op = 255;
	#endif
	while (1) {
		uint8_t op = read_byte(task);
		//printf("%s %zu %s\n", task->frame->name, task->frame->iptr, get_opname(op));

		switch (op) {
			case OP_NUM: {
				double num = read_num(task);
				push_num(task, num);
			} break;
			case OP_STR: {
				const char* str = read_str(task);
				push_str(task, gc_strdup(str));
			} break;
			case OP_ADD:
			case OP_SUB:
			case OP_MUL:
			case OP_DIV:
			case OP_MOD: 
			case OP_GT:
			case OP_LT: 
			case OP_GE:
			case OP_LE: 
			case OP_EQ:
			case OP_NE: {
				if (op != OP_EQ && op != OP_NE) {
					task->frame->ln = read_addr(task);
				}
				Object* o2 = pop_value(task);
				Object* o1 = pop_value(task);

				if (o1->kind == TABLE && o1->metatable->kind == TABLE) {
					Table* mtable = o1->metatable->table;

					const char* method_name = NULL;
					switch (op) {
						case OP_ADD: method_name = "__add"; break;
						case OP_SUB: method_name = "__sub"; break;
						case OP_MUL: method_name = "__mul"; break;
						case OP_DIV: method_name = "__div"; break;
						case OP_MOD: method_name = "__mod"; break;
						case OP_GT: method_name = "__gt"; break;
						case OP_LT: method_name = "__lt"; break;
						case OP_GE: method_name = "__ge"; break;
						case OP_LE: method_name = "__le"; break;
						case OP_EQ: method_name = "__eq"; break;
						case OP_NE: method_name = "__ne"; break;
					}

					Object* func = table_get(mtable, tug_conststr(method_name));
					if (func != obj_nil) {
						Vector* args = vec_serve(2);
						vec_push(args, o1);
						vec_push(args, o2);

						Object* args_obj = obj_create(TUPLE);
						args_obj->tuple = args;
						gc_collect_obj(args_obj);

						Object* res = tug_call(task, func, args_obj);
						switch (op) {
							case OP_GT:
							case OP_LT:
							case OP_GE:
							case OP_LE:
							case OP_EQ:
							case OP_NE: {
								if (tug_gettype(res) != TRUE && tug_gettype(res) != FALSE && tug_gettype(res) != NIL) {
									assign_err(task, "metamethod '%s' must return 'bool', got '%s'", method_name, obj_type(res));
								} else {
									push_obj(task, res);
								}
							} break;
						}
						break;
					}
				} else if (op == OP_EQ || op == OP_NE) {
					int res = obj_compare(o1, o2);
					if (op == OP_NE) res = !res;
					push_obj(task, obj_truth(res));
				} else if (o1->kind == NUM && o2->kind == NUM) {
					double n1 = o1->num;
					double n2 = o2->num;

					switch (op) {
						case OP_ADD: push_num(task, n1 + n2); break;
						case OP_SUB: push_num(task, n1 - n2); break;
						case OP_MUL: push_num(task, n1 * n2); break;
						case OP_DIV: {
							if (n2 == 0.0) {
								assign_err(task, "zero division");
							} else {
								push_num(task, n1 / n2);
							}
						} break;
						case OP_MOD: {
							if (n2 == 0.0) {
								assign_err(task, "zero modulo");
							} else {
								push_num(task, fmod(n1, n2));
							}
						} break;
						case OP_GT: {
							push_obj(task, obj_truth(n1 > n2));
						} break;
						case OP_LT: {
							push_obj(task, obj_truth(n1 < n2));
						} break;
						case OP_GE: {
							push_obj(task, obj_truth(n1 >= n2));
						} break;
						case OP_LE: {
							push_obj(task, obj_truth(n1 <= n2));
						} break;
					}
				} else if (
					o1->kind == STR && o2->kind == STR
					&& (
						op == OP_ADD || op == OP_GT
						|| op == OP_LT || op == OP_GE
						|| op == OP_LE
					)
				) {
					switch (op) {
						case OP_GT: {
							push_obj(task, obj_truth(strcmp(o1->str, o2->str) > 0));
						} break;
						case OP_LT: {
							push_obj(task, obj_truth(strcmp(o1->str, o2->str) < 0));
						} break;
						case OP_GE: {
							push_obj(task, obj_truth(strcmp(o1->str, o2->str) >= 0));
						} break;
						case OP_LE: {
							push_obj(task, obj_truth(strcmp(o1->str, o2->str) <= 0));
						} break;
						case OP_ADD: {
							size_t len1 = strlen(o1->str);
							size_t len2 = strlen(o2->str);

							char* res = gc_malloc(len1 + len2 + 1);
							memcpy(res, o1->str, len1);
							memcpy(res + len1, o2->str, len2);
							res[len1 + len2] = '\0';
							push_str(task, res);
						} break;
					}
				} else {
					char* op_s;
					switch (op) {
						case OP_ADD: op_s = "add"; break;
						case OP_SUB: op_s = "sub"; break;
						case OP_MUL: op_s = "mul"; break;
						case OP_DIV: op_s = "div"; break;
						case OP_MOD: op_s = "mod"; break;
						case OP_GT: op_s = "gt"; break;
						case OP_LT: op_s = "lt"; break;
						case OP_GE: op_s = "ge"; break;
						case OP_LE: op_s = "le"; break;
					}
					assign_err(task, "unable to %s '%s' with '%s'", op_s, obj_type(o1), obj_type(o2));
				}
			} break;
			case OP_HALT: {
				Frame* next_frame = task->frame->next;
				frame_free(task->frame, NULL);
				task->frame = next_frame;
				task->frame_count--;
				set_ret(task, peek_tvalue(task));
				if (task->frame == NULL) task->state = TASK_END;
				else {
					task->frame->protected = 0;
				}

				vec_pop(task->varmaps);
				return;
			}

			case OP_TRUE: push_obj(task, obj_true); break;
			case OP_FALSE: push_obj(task, obj_false); break;
			case OP_NIL: push_obj(task, obj_nil); break;

			#if TUG_DEBUG

			case OP_DEBUG_PRINT: {
				Object* obj = pop_value(task);
				obj_print(obj);
			} break;

			#endif

			case OP_POP: {
				size_t count = read_addr(task);
				for (size_t i = 0; i < count; i++) pop_value(task);
			} break;

			case OP_JUMPT: {
				size_t addr = read_addr(task);
				uint8_t pback = read_byte(task);
				Object* obj = pop_value(task);
				if (obj_check(obj)) {
					set_addr(task, addr);
				}
				if (pback) push_obj(task, obj);
			} break;

			case OP_JUMPF: {
				size_t addr = read_addr(task);
				uint8_t pback = read_byte(task);
				Object* obj = pop_value(task);
				if (!obj_check(obj)) {
					set_addr(task, addr);
				}
				if (pback) push_obj(task, obj);
			} break;

			case OP_VAR: {
				const char* name = read_str(task);
				push_obj(task, get_var(task, name));
			} break;

			case OP_STORE: {
				uint8_t local = read_byte(task);
				size_t count = read_addr(task);

				for (size_t i = 0; i < count; i++) {
					const char* name = read_str(task);
					Object* value = pop_value(task);
					if (local) {
						set_var(task, name, value);
					} else {
						edit_var(task, name, value);
					}
				}
			} break;

			case OP_POS:
			case OP_NEG:
			case OP_NOT: {
				if (op != OP_NOT) {
					task->frame->ln = read_addr(task);
				}

				Object* obj = pop_value(task);

				int err = 0;
				if (obj->kind == TABLE && obj->metatable != obj_nil) {
					Table* mtable = obj->metatable->table;
					Object* func;
					if (op == OP_NOT) func = table_get(mtable, tug_conststr("__truth"));
					else if (op == OP_POS) func = table_get(mtable, tug_conststr("__pos"));
					else func = table_get(mtable, tug_conststr("__neg"));

					if (func != obj_nil) {
						Vector* args = vec_serve(1);
						vec_push(args, obj);

						call_fobj(func, args);

						if (op == OP_NOT) {
							Object* obj = pop_value(task);
							if (obj == obj_true) push_obj(task, obj_nil);
							else if (obj == obj_false || obj == obj_nil) push_obj(task, obj_true);
							else assign_err(task, "metamethod '__truth' must return 'bool', got '%s'", obj_type(obj));
						}
					} else err = 1;
				} else if (op == OP_NOT) {
					push_obj(task, obj_truth(!obj_check(obj)));
				} else {
					if (obj->kind == NUM) {
						push_num(task, op == OP_NEG ? -(obj->num) : obj->num);
					} else err = 1;
				}

				if (err) assign_err(task, "unable to %s '%s'", op == OP_ADD ? "pos" : "neg", obj_type(obj));
			} break;

			case OP_JUMP: set_addr(task, read_addr(task)); break;

			case OP_PUSH_CLOSURE: {
				VarMap* map = get_map(task);
				VarMap* newmap = varmap_create();
				newmap->next = map;
				vec_set(task->varmaps, vec_count(task->varmaps) - 1, newmap);

				gc_collect_closure(newmap);
			} break;

			case OP_POP_CLOSURE: {
				VarMap* map = get_map(task);
				map = map->next;
			} break;

			case OP_JUMPP: {
				size_t count = read_addr(task);
				for (size_t i = 0; i < count; i++) {
					VarMap* map = get_map(task);
					map = map->next;
				}
				set_addr(task, read_addr(task));
			} break;

			case OP_FUNCDEF: {
				size_t ln = read_addr(task);
				task->frame->ln = ln;

				size_t namec = read_addr(task);
				char* name = NULL;
				Object* obj = NULL;
				char* lastname = NULL;
				if (namec == 0) name = gc_strdup("<anonymous>");
				else {
					name = gc_malloc(256);
					size_t cap = 256;
					size_t len = 0;
					for (size_t i = 0; i < namec; i++) {
						const char* part = read_str(task);
						while (len + strlen(part) + 2 > cap) {
							cap *= 2;
							name = gc_realloc(name, cap);
						}
						if (i == namec - 1) lastname = gc_strdup(part);
						if (i == 0) {
							obj = get_var(task, part);

							strcpy(name, part);
							len += strlen(part);
						} else {
							if (i != namec - 1) {
								Object* mmethod = tuglib_getmetafield(obj, "__get");
								if (mmethod != obj_nil) {
									Vector* args = vec_serve(2);
									vec_push(args, obj);
									vec_push(args, tug_conststr(part));

									call_fobj(mmethod, args);
									Object* ret = pop_tvalue(task);
									if (ret->kind == TUPLE) {
										ret = vec_get(ret->tuple, 0);
									}
									obj = ret;
								} else if (obj->kind == TABLE) {
									obj = table_get(obj->table, tug_conststr(part));
								} else {
									assign_err(task, "unable to get index '%s'", obj_type(obj));
									gc_free(name);
									break;
								}
							}

							strcat(name, ".");
							strcat(name, part);
							len += strlen(part) + 1;
						}
					}
				}

				size_t count = read_addr(task);
				Vector* params = vec_create();
				for (size_t i = 0; i < count; i++) {
					vec_push(params, (char*)read_str(task));
				}

				Bytecode* bc = read_bc(task);
				Object* fobj = new_func(task, name, params, bc);
				if (obj && namec > 1) {
					Object* mmethod = tuglib_getmetafield(obj, "__set");
					if (mmethod != obj_nil) {
						Vector* args = vec_serve(3);
						vec_push(args, obj);
						vec_push(args, new_str(lastname));
						vec_push(args, fobj);

						call_fobj(mmethod, args);
						pop_value(task);
					} else if (obj->kind == TABLE) {
						table_set(obj->table, new_str(lastname), fobj);
					} else {
						assign_err(task, "unable to set function to field '%s'", obj_type(obj));
					}
				} else vec_push(task->stack, fobj);
				vec_free(params);
			} break;

			case OP_CALL: {
				size_t arg_count = read_addr(task);
				size_t ln = read_addr(task);
				task->frame->ln = ln;

				Vector* args = vec_serve(arg_count);
				for (size_t i = 0; i < arg_count; i++) {
					vec_pushfirst(args, pop_value(task));
				}

				Object* obj = pop_value(task);
				call_obj(task, obj, args, 1, 0);
			} break;

			case OP_TUPLE: {
				size_t count = read_addr(task);
				Vector* tuple = vec_serve(count);

				for (size_t i = 0; i < count; i++) {
					Object* obj = pop_tvalue(task);
					if (obj->kind == TUPLE) {
						for (size_t i = vec_count(obj->tuple); i-- > 0;) {
							vec_pushfirst(tuple, vec_get(obj->tuple, i));
						}
					} else vec_pushfirst(tuple, obj);
				}

				Object* obj = obj_create(TUPLE);
				obj->tuple = tuple;

				push_obj(task, obj);
				gc_collect_obj(obj);
			} break;

			case OP_TABLE: {
				push_newtable(task);
			} break;

			case OP_SETINDEX: {
				task->frame->ln = read_addr(task);
				uint8_t push = read_byte(task);
				Object* value = pop_value(task);
				Object* key = pop_value(task);
				Object* obj = pop_value(task);

				if (obj->kind == TABLE) {
					Table* table = obj->table;
					int meta = 0;
					if (obj->metatable != obj_nil) {
						Table* mtable = obj->metatable->table;
						Object* func = table_get(mtable, tug_conststr("__set"));

						if (func != obj_nil) {
							Vector* args = vec_serve(3);
							vec_push(args, obj);
							vec_push(args, key);
							vec_push(args, value);

							call_fobj(func, args);
							pop_value(task);
							meta = 1;
						}
					}

					if (!meta) table_set(table, key, value);
				} else if (obj->kind == LIST) {
					Vector* lvec = obj->list;
					if (key->kind != NUM) {
						assign_err(task, "unable to set list index with '%s'", obj_type(key));
						break;
					}

					long idx = (long)key->num;
					if (idx < 0 || idx >= vec_count(lvec)) {
						assign_err(task, "set index out of range");
						break;
					}
					lvec->array[idx] = value;
				} else {
					assign_err(task, "unable to set index '%s'", obj_type(obj));
					break;
				}

				if (push) push_obj(task, obj);
			} break;

			case OP_GETINDEX: {
				task->frame->ln = read_addr(task);
				Object* key = pop_value(task);
				Object* obj = pop_value(task);

				if (obj->kind == TABLE) {
					Table* table = obj->table;
					int meta = 0;

					if (obj->metatable != obj_nil) {
						Table* mtable = obj->metatable->table;
						Object* func = table_get(mtable, tug_conststr("__get"));

						if (func != obj_nil) {
							Vector* args = vec_serve(3);
							vec_push(args, obj);
							vec_push(args, key);

							call_obj(task, func, args, 1, 0);
							meta = 1;
						}
					}

					if (!meta) push_obj(task, table_get(table, key));
				} else if (obj->kind == STR && key->kind == NUM) {
					long idx = (long)key->num;
					if (idx < 0) {
						push_obj(task, obj_nil);
						break;
					}
					size_t len = strlen(obj->str);
					if (idx > len) {
						push_obj(task, obj_nil);
						break;
					}
					
					char res[2];
					res[0] = obj->str[idx];
					res[1] = '\0';
					push_obj(task, tug_conststr(res));
				} else if (obj->kind == LIST && key->kind == NUM) {
					long idx = (long)key->num;
					if (idx < 0) {
						push_obj(task, obj_nil);
						break;
					}
					size_t len = obj->list->count;
					if (idx > len) {
						push_obj(task, obj_nil);
						break;
					}
					
					push_obj(task, vec_get(obj->list, idx));
				} else {
					assign_err(task, "unable to get index '%s' with '%s'", obj_type(obj), obj_type(key));
				}
			} break;

			case OP_MULTIASSIGN: {
				task->frame->ln = read_addr(task);
				uint8_t local = read_byte(task);
				size_t value_count = read_addr(task);
				size_t assign_count = read_addr(task);

				Vector* objects = pop_nvalue(task, assign_count);
				if (assign_count < value_count) {
					for (size_t i = assign_count; i < value_count; i++) {
						pop_tvalue(task);
					}
				}
				Vector* leftside = vec_create();
				ui8_array* kinds = ui8_array_create();
				for (size_t i = 0; i < assign_count; i++) {
					uint8_t kind = read_byte(task);

					if (kind) vec_push(leftside, (void*)read_str(task));
					else vec_push(leftside, pop_nvalue(task, 2));
					ui8_array_push(kinds, kind);
				}

				uint8_t err = 0;
				for (size_t i = 0; i < assign_count; i++) {
					size_t ri = assign_count - i - 1;
					uint8_t kind = ui8_array_get(kinds, ri);
					Object* value = vec_get(objects, i);

					if (!err) {
						if (kind) {
							const char* name = vec_get(leftside, ri);
							if (local) {
								set_var(task, name, value);
							} else {
								edit_var(task, name, value);
							}
						} else {
							Vector* obj_key = vec_get(leftside, ri);
							Object* obj = vec_get(obj_key, 0);
							Object* key = vec_get(obj_key, 1);

							if (obj->kind == TABLE) {
								int meta = 0;
								if (obj->metatable != obj_nil) {
									Table* mtable = obj->metatable->table;
									Object* func = table_get(mtable, tug_conststr("__set"));

									if (func != obj_nil) {
										Vector* args = vec_serve(3);
										vec_push(args, obj);
										vec_push(args, key);
										vec_push(args, value);

										call_fobj(func, args);
										err = task->state == TASK_ERROR;
										pop_value(task);
										meta = 1;
									}
								}

								if (!meta) table_set(obj->table, key, value);
							} else if (obj->kind == LIST) {
								Vector* lvec = obj->list;
								if (key->kind != NUM) {
									assign_err(task, "unable to set list index with '%s'", obj_type(key));
									err = 1;
								} else {
									long idx = (long)key->num;
									if (idx < 0 || idx >= vec_count(lvec)) {
										assign_err(task, "set index out of range");
										err = 1;
									} else lvec->array[idx] = value;
								}
							} else {
								assign_err(task, "unable to set index '%s'", obj_type(obj));
								err = 1;
							}
						}
					}

					if (!kind) {
						vec_free((Vector*)(vec_get(leftside, i)));
					}
				}

				vec_free(objects);
				vec_free(leftside);
				ui8_array_free(kinds);
			} break;

			case OP_ITER: {
				task->frame->ln = read_addr(task);
				Object* obj = pop_value(task);
				int meta = 0;
				if (obj->kind == TABLE && obj->metatable != obj_nil) {
					Table* mtable = obj->metatable->table;
					Object* func = table_get(mtable, tug_conststr("__iter"));

					if (func != obj_nil) {
						Vector* args = vec_serve(1);
						vec_push(args, obj);

						Object* args_obj = obj_create(TUPLE);
						args_obj->tuple = args;
						gc_collect_obj(args_obj);

						obj = tug_call(task, func, args_obj);
						meta = 1;
					}
				}
				Object* iter_obj = obj_iter(obj);
				if (iter_obj == NULL) {
					if (meta) {
						assign_err(task, "metamethod '__iter' must return an iterable, got '%s'", obj_type(obj));
					} else {
						assign_err(task, "unable to iterate '%s'", obj_type(obj));
					}
				} else {
					push_obj(task, gc_obj(iter_obj));
				}
			} break;

			case OP_NEXT: {
				task->frame->ln = read_addr(task);
				size_t count = read_addr(task);
				Vector* names = vec_serve(count);
				for (size_t i = 0; i < count; i++) {
					vec_push(names, (char*)read_str(task));
				}
				size_t pos = read_addr(task);

				Object* iter_obj = peek_value(task);
				int done = 0;
				int used = 0;
				if (iter_obj->kind == ITER_STR) {
					if (iter_obj->iter.idx < iter_obj->iter.len) {
						char str[2];
						str[0] = iter_obj->iter.obj->str[iter_obj->iter.idx++];
						str[1] = '\0';
						set_var(task, vec_get(names, 0), tug_conststr(str));
						used = 1;
					} else {
						done = 1;
					}
				} else if (iter_obj->kind == ITER_TABLE) {
					Object* table_obj = iter_obj->iter.obj;
					Table* table = table_obj->table;
					if (iter_obj->iter.entry == NULL) {
						while (iter_obj->iter.idx < table->capacity && !table->buckets[iter_obj->iter.idx]) {
							iter_obj->iter.idx++;
						}
						iter_obj->iter.entry = table->buckets[iter_obj->iter.idx];
					}
					if (iter_obj->iter.entry == NULL) done = 1;
					else {
						set_var(task, vec_get(names, 0), iter_obj->iter.entry->key);
						if (vec_count(names) >= 2) {
							set_var(task, vec_get(names, 1), iter_obj->iter.entry->value);
						}
						iter_obj->iter.entry = iter_obj->iter.entry->next;
						while (iter_obj->iter.entry == NULL && ++iter_obj->iter.idx < table->capacity) {
							iter_obj->iter.entry = table->buckets[iter_obj->iter.idx];
						}
						used = 2;
					}
				} else if (iter_obj->kind == ITER_LIST) {
					Vector* list = iter_obj->iter.obj->list;
					if (iter_obj->iter.idx < vec_count(list)) {
						set_var(task, vec_get(names, 0), vec_get(list, iter_obj->iter.idx++));
						used = 1;
					} else done = 1;
				} else {
					Object* func = tuglib_getmetafield(iter_obj, "__next");

					if (func != obj_nil) {
						Vector* args = vec_serve(1);
						vec_push(args, iter_obj);
						
						call_fobj(func, args);
						
						Object* ret = pop_tvalue(task);
						Object* dobj;
						if (ret->kind == TUPLE) {
							dobj = vec_get(ret->tuple, 0);
							for (size_t i = 0; i < vec_count(names); i++) {
								size_t j = i + 1;
								if (j >= vec_count(ret->tuple)) break;
								set_var(task, (const char*)vec_get(names, i), vec_get(ret->tuple, j));
								used++;
							}
						} else dobj = ret;

						if (dobj == obj_nil || dobj == obj_false) {
							done = 1;
						} else if (dobj != obj_true) assign_err(task, "metamethod '__next' must return 'bool' or 'nil', got '%s'", obj_type(dobj));
					} else assign_err(task, "iteration fatal error");
				}

				if (done) {
					set_addr(task, pos);
					pop_value(task);
				} else {
					for (size_t i = used; i < vec_count(names); i++) {
						const char* name = (const char*)vec_get(names, i);
						set_var(task, name, obj_nil);
					}
				}
				vec_free(names);
			} break;
			
			case OP_LIST: {
				size_t count = read_addr(task);
				Vector* list = vec_create();
				for (size_t i = 0; i < count; i++) {
					vec_pushfirst(list, pop_value(task));
				}
				
				Object* obj = gc_obj(obj_create(LIST));
				obj->list = list;
				push_obj(task, obj);
			} break;
		}

		if (task->state == TASK_ERROR) {
			while (task->frame) {
				Frame* frame = task->frame;

				if (frame->protected == 1) {
					info_free(task->info);
					task->info = NULL;
					frame->protected = 0;
					break;
				} else {
					task->stack->count = frame->base;
					task->varmaps->count = frame->scope;
				}

				Frame* next = frame->next;
				frame_free(frame, &task->info);
				task->frame = next;
				task->frame_count--;
			}
		}

		if (task->state != TASK_RUNNING) break;
		#ifdef TUG_DEBUG
		prev_op = op;
		#endif

		gc_run();
	}
}

static void task_run(Task* task) {
	task->state = TASK_RUNNING;

	while (task->state == TASK_RUNNING) {
		task_exec(task);
	}
}

static void task_resume(Task* task) {
	if (task->state == TASK_NEW || task->state == TASK_YIELD) {
		task_run(task);
	}
}

static void task_report(Task* task) {
	if (task->info != NULL) {
		printf("stack traceback:\n");
		Info* info = task->info;
		while (info) {
			if (streq(info->src, "[C]")) {
				printf("\t%s: in %s\n", info->src, info->name);
			} else {
				printf("\t%s:%lu: in %s\n", info->src, info->ln, info->name);
			}
			info = info->next;
		}
	}

	printf("error: %s\n", task->msg);
}

// task will be closed but not yet freed
// it's GC managed
static void task_close(Task* task) {
	Frame* frame = task->frame;
	while (frame) {
		Frame* next = frame->next;
		frame_free(frame, NULL);
		frame = next;
	}
	task->state = TASK_END;

	vec_free(task->varmaps);
	vec_free(task->stack);
	task->stack = NULL;
	info_free(task->info);
}

typedef struct GCBlock {
	struct GCBlock* next;
} GCBlock;

struct {
	size_t count;
	size_t capacity;
	GCBlock* list;
	void** chunks;
	size_t ccount;
} pool;

typedef struct {
	size_t size;
} GCHeader;

static size_t gc_size;
static size_t threshold;
static Vector* objects;
static Vector* closures;
static Vector* tasks;

static void gc_init() {
	objects = vec_create();
	closures = vec_create();
	tasks = vec_create();
	gc_size = 0;
	threshold = 1024 * 1024;
}

static void* gc_malloc(size_t size) {
	GCHeader* header = malloc(sizeof(GCHeader) + size);
	header->size = size;
	gc_size += size;
	return (void*)(header + 1);
}

static void* gc_realloc(void* ptr, size_t new_size) {
	if (!ptr) return gc_malloc(new_size);

	GCHeader* header = ((GCHeader*)ptr) - 1;
	size_t old_size = header->size;
	header = realloc(header, sizeof(GCHeader) + new_size);
	header->size = new_size;
	gc_size += new_size - old_size;

	return (void*)(header + 1);
}

static void* gc_calloc(size_t nmemb, size_t size) {
	size_t total = nmemb * size;
	return memset(gc_malloc(total), 0, total);
}

static void gc_free(void* ptr) {
	if (!ptr) return;
	GCHeader* header = ((GCHeader*)ptr) - 1;
	gc_size -= header->size;
	free(header);
}

static char* gc_strdup(const char* str) {
	size_t len = strlen(str) + 1;
	return memcpy(gc_malloc(len), str, strlen(str) + 1);
}

static inline void gc_collect_obj(Object* obj) {
	if (obj->collected) return;
	obj->collected = 1;
	vec_push(objects, obj);
}

static inline void gc_collect_closure(VarMap* varmap) {
	vec_push(closures, varmap);
}

static inline void gc_collect_task(Task* task) {
	vec_push(tasks, task);
}

static void gc_mark_closure(VarMap* varmap);
static void gc_mark_obj(Object* obj) {
	if (!obj || obj == obj_true || obj == obj_false || obj == obj_nil) return;
	if (obj->marked) return;
	obj->marked = 1;
	if (obj->kind == TUPLE) {
		for (size_t i = 0; i < vec_count(obj->tuple); i++) {
			gc_mark_obj(obj);
		}
	} else if (obj->kind == TABLE) {
		Table* table = obj->table;
		for (size_t i = 0; i < table->capacity; i++) {
			TableEntry* entry = table->buckets[i];

			while (entry) {
				gc_mark_obj(entry->key);
				gc_mark_obj(entry->value);
				entry = entry->next;
			}
		}
		
		gc_mark_obj(obj->metatable);
	} else if (obj->kind == ITER_STR || obj->kind == ITER_TABLE || obj->kind == ITER_LIST) gc_mark_obj(obj->iter.obj);
	else if (obj->kind == FUNC) {
		VarMap* map = obj->func.upper;
		while (map) {
			gc_mark_closure(map);
			map = map->next;
		}
	} else if (obj->kind == LIST) {
		for (size_t i = 0; i < vec_count(obj->list); i++) {
			gc_mark_obj(vec_get(obj->list, i));
		}
	}
}

static void gc_mark_closure(VarMap* varmap) {
	if (!varmap || varmap->marked) return;
	varmap->marked = 1;
	for (size_t i = 0; i < varmap->capacity; i++) {
		VarMapEntry* entry = varmap->buckets[i];
		while (entry) {
			gc_mark_obj(entry->value);
			entry = entry->next;
		}
	}
}

static void gc_mark_task(Task* task) {
	if (!task || task->state == TASK_END) return;

	vec_iter(task->stack, gc_mark_obj);

	for (size_t i = 0; i < vec_count(task->varmaps); i++) {
		VarMap* map = vec_get(task->varmaps, i);
		while (map) {
			gc_mark_closure(map);
			map = map->next;
		}
	}
	
	gc_mark_closure(task->global);
	
	Frame* frame = task->frame;
	while (frame) {
		Vector* args = frame->args;
		if (args) {
			for (size_t i = 0; i < vec_count(args); i++) {
				gc_mark_obj(vec_get(args, i));
			}
		}
		gc_mark_obj(frame->ret);
		frame = frame->next;
	}
}

static void gc_sweep(void) {
	size_t count = 0;
	for (size_t i = 0; i < vec_count(objects); i++) {
		Object* obj = vec_get(objects, i);
		if (!obj->marked) {
			obj_free(obj);
		} else {
			obj->marked = 0;
			vec_set(objects, count++, obj);
		}
	}
	objects->count = count;

	count = 0;
	for (size_t i = 0; i < vec_count(closures); i++) {
		VarMap* varmap = vec_get(closures, i);
		if (!varmap->marked) {
			varmap_free(varmap);
		} else {
			varmap->marked = 0;
			vec_set(closures, count++, varmap);
		}
	}
	closures->count = count;

	count = 0;
	for (size_t i = 0; i < vec_count(tasks); i++) {
		Task* task = vec_get(tasks, i);
		if (task->state == TASK_END) {
			task_close(task);
			gc_free(task);
		} else {
			vec_set(tasks, count++, task);
		}
	}

	size_t ssize = gc_size;
	size_t old_threshold = threshold;

	double desired = (double)gc_size / TUG_TARGET_UNTIL;

	double min_allowed = (double)old_threshold / 2.0 * TUG_MIN_SHRINK;
	double max_allowed = (double)old_threshold * TUG_MAX_GROWTH;
	if (desired < min_allowed) desired = min_allowed;
	else if (desired > max_allowed) desired = max_allowed;

	threshold = (size_t)desired;
}

static inline void gc_run(void) {
	if (gc_size < threshold) return;

	vec_iter(tasks, gc_mark_task);
	gc_sweep();
}

static inline void gc_close(void) {
	gc_sweep();
	vec_free(objects);
	vec_free(closures);
	vec_free(tasks);
}

// API

tug_Object* tug_true = obj_true;
tug_Object* tug_false = obj_false;
tug_Object* tug_nil = obj_nil;

tug_Object* tug_str(char* str) {
	Object* obj = new_str(str);
	obj->m = 1;
	return obj;
}

tug_Object* tug_conststr(const char* str) {
	return new_str(gc_strdup(str));
}

tug_Object* tug_num(double num) {
	return new_num(num);
}

tug_Object* tug_table(void) {
	return new_table();
}

tug_Object* tug_cfunc(const char* name, tug_CFunc func) {
	return gc_obj(obj_cfunc(name, func));
}

tug_Object* tug_tuple(void) {
	Object* obj = obj_create(TUPLE);
	obj->tuple = vec_create();

	return gc_obj(obj);
}

void tug_tuplepush(tug_Object* tuple, tug_Object* obj) {
	if (obj->kind == TUPLE) {
		for (size_t i = 0; i < vec_count(obj->tuple); i++) {
			vec_push(tuple->tuple, vec_get(obj->tuple, i));
		}
		return;
	}
	vec_push(tuple->tuple, obj);
}

tug_Object* tug_tuplepop(tug_Object* tuple) {
	tug_Object* obj = vec_pop(tuple->tuple);

	return obj ? obj : obj_nil;
}

tug_Object* tug_list(void) {
	Object* obj = gc_obj(obj_create(LIST));
	obj->list = vec_create();

	return obj;
}

void tug_listpush(tug_Object* list, tug_Object* obj) {
	vec_push(list->list, obj);
}

tug_Object* tug_listpop(tug_Object* list, size_t idx) {
	Vector* lvec = list->list;
	if (idx >= lvec->count) return obj_nil;
	if (lvec->count == 0) return obj_nil;
	
	Object* obj = lvec->array[idx];
	memmove(&lvec->array[idx], &lvec->array[idx + 1], (lvec->count - idx - 1) * sizeof(void*));
	lvec->count--;
	vec_dynamic(lvec);
	return obj;
}

void tug_listinsert(tug_Object* list, size_t idx, tug_Object* obj) {
	Vector* lvec = list->list;
	if (idx > lvec->count) {
		vec_push(lvec, obj);
		return;
	}
	
	vec_dynamic(lvec);
	memmove(&lvec->array[idx + 1], &lvec->array[idx], (lvec->count - idx) * sizeof(void*));
	lvec->array[idx] = obj;
	lvec->count++;
	return;
}

int tug_listset(tug_Object* list, size_t idx, tug_Object* obj) {
	Vector* lvec = list->list;
	if (idx >= lvec->count) return 0;
	lvec->array[idx] = obj;
	return 1;
}

unsigned long tug_getid(tug_Object* obj) {
	return obj->id;
}

tug_Type tug_gettype(tug_Object* obj) {
	switch (obj->kind) {
		case STR: return TUG_STR;
		case NUM: return TUG_NUM;
		case TRUE: return TUG_TRUE;
		case FALSE: return TUG_FALSE;
		case NIL: return TUG_NIL;
		case FUNC: return TUG_FUNC;
		case TABLE: return TUG_TABLE;
		case LIST: return TUG_LIST;
		case TUPLE: return tug_gettype(obj->tuple->count > 0 ? vec_get(obj->tuple, 0) : obj_nil);
		default: return TUG_UNKNOWN;
	}
}

const char* tug_getstr(tug_Object* obj) {
	return (const char*)obj->str;
}

double tug_getnum(tug_Object* obj) {
	return obj->num;
}

void tug_setfield(tug_Object* obj, tug_Object* key, tug_Object* value) {
	table_set(obj->table, key, value);
}

tug_Object* tug_getfield(tug_Object* obj, tug_Object* key) {
	return table_get(obj->table, key);
}

size_t tug_getlen(tug_Object* obj) {
	return obj->kind == STR ? strlen(obj->str) : obj->kind == TABLE ? obj->table->count : obj->kind == LIST ? obj->list->count : 0;
}

void tug_setmetatable(tug_Object* obj, tug_Object* metatable) {
	obj->metatable = metatable;
}

tug_Object* tug_getmetatable(tug_Object* obj) {
	return obj->metatable;
}

void tug_setvar(tug_Task* T, const char* name, tug_Object* value) {
	VarMap* map = vec_peek(T->varmaps);
	varmap_put(map, name, (Object*)value);
}

tug_Object* tug_getvar(tug_Task* T, const char* name) {
	VarMap* map = vec_peek(T->varmaps);
	tug_Object* res = (tug_Object*)varmap_get(map, name);

	return res ? res : obj_nil;
}

int tug_hasvar(tug_Task* T, const char* name) {
	VarMap* map = vec_peek(T->varmaps);
	tug_Object* res = (tug_Object*)varmap_get(map, name);

	return res ? 1 : 0;
}

void tug_setglobal(tug_Task* T, const char* name, tug_Object* value) {
	varmap_put(T->global, name, value);
}

tug_Object* tug_getglobal(tug_Task* T, const char* name) {
	VarMap* map = T->global;
	tug_Object* res = (tug_Object*)varmap_get(map, name);

	return res ? res : obj_nil;
}

int tug_hasglobal(tug_Task* T, const char* name) {
	VarMap* map = T->global;
	tug_Object* res = (tug_Object*)varmap_get(map, name);

	return res ? 1 : 0;
}

size_t tug_getargc(tug_Task* T) {
	return get_argc(T);
}

tug_Object* tug_getarg(tug_Task* T, size_t idx) {
	return get_arg(T, idx);
}

int tug_hasarg(tug_Task* T, size_t idx) {
	Frame* frame = T->frame;
	if (frame->args == NULL) return 0;
	if (idx >= frame->args->count) return 0;

	return 1;
}

tug_Object* tug_calls(tug_Task* T, tug_Object* func, size_t n, ...) {
	va_list args;
	va_start(args, n);

	Vector* fargs = vec_serve(n);
	for (size_t i = 0; i < n; i++) {
		vec_push(fargs, va_arg(args, tug_Object*));
	}

	call_obj(T, func, fargs, 1, 0);
	if (T->state != TASK_ERROR) task_exec(T);
	pop_value(T);
	return get_ret(T);
}

tug_Object* tug_pcalls(tug_Task* T, int* errptr, tug_Object* func, size_t n, ...) {
	va_list args;
	va_start(args, n);

	Vector* fargs = vec_serve(n);
	for (size_t i = 0; i < n; i++) {
		vec_push(fargs, va_arg(args, tug_Object*));
	}

	call_obj(T, func, fargs, 1, 1);
	if (T->state != TASK_ERROR) task_exec(T);
	if (errptr) (*errptr) = (T->state == TASK_ERROR);
	if (T->state == TASK_ERROR) {
		T->state = TASK_RUNNING;
	}
	pop_value(T);
	return get_ret(T);
}

tug_Object* tug_call(tug_Task* T, tug_Object* func, tug_Object* arg) {
	Vector* args;
	if (arg->kind == TUPLE) {
		args = arg->tuple;
	} else {
		args = vec_serve(1);
		vec_push(args, arg);
	}

	call_obj(T, func, args, 0, 0);
	if (T->state != TASK_ERROR) task_exec(T);
	pop_value(T);
	return get_ret(T);
}

tug_Object* tug_pcall(tug_Task* T, int* errptr, tug_Object* func, tug_Object* arg) {
	Vector* args;
	if (arg->kind == TUPLE) {
		args = arg->tuple;
	} else {
		args = vec_serve(1);
		vec_push(args, arg);
	}

	call_obj(T, func, args, 0, 1);
	if (T->state != TASK_ERROR) task_exec(T);
	if (errptr) (*errptr) = (T->state == TASK_ERROR);
	if (T->state == TASK_ERROR) {
		T->state = TASK_RUNNING;
	}
	pop_value(T);
	return get_ret(T);
}

void tug_rets(tug_Task* T, size_t n, ...) {
	Vector* stack = T->stack;
	if (n == 0) {
		T->frame->ret = obj_nil;
		return;
	}

	va_list args;
	va_start(args, n);

	if (n == 1) {
		T->frame->ret = va_arg(args, Object*);
		va_end(args);
		return;
	}

	Vector* tuple = vec_serve(n);
	for (size_t i = 0; i < n; i++) {
		vec_push(tuple, va_arg(args, Object*));
	}
	Object* obj = gc_obj(obj_create(TUPLE));
	obj->tuple = tuple;
	T->frame->ret = obj;

	va_end(args);
}

void tug_ret(tug_Task* T, tug_Object* obj) {
	T->frame->ret = obj;
}

void tug_err(tug_Task* T, const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);
	vsnprintf(T->msg, sizeof(T->msg), fmt, args);
	va_end(args);
	T->state = TASK_ERROR;

	longjmp(cfunc_jmp_buf, 1);
}

static void append_str(char** buf, size_t* len, const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);

	char tmp[2048];
	int n = vsnprintf(tmp, sizeof(tmp), fmt, args);
	va_end(args);

	if (n < 0) return;

	*buf = gc_realloc(*buf, *len + n + 1);
	memcpy(*buf + *len, tmp, n);
	*len += n;
	(*buf)[*len] = '\0';
}

const char* tug_getmsg(tug_Task* T) {
	return T->msg;
}

const char* tug_geterr(tug_Task* T) {
	char* buf = NULL;
	size_t len = 0;

	if (T->info != NULL) {
		append_str(&buf, &len, "stack traceback:\n");
		Info* info = T->info;
		while (info) {
			if (streq(info->src, "[C]")) {
				append_str(&buf, &len, "\t%s: in %s\n", info->src, info->name);
			} else {
				append_str(&buf, &len, "\t%s:%lu: in %s\n", info->src, info->ln, info->name);
			}
			info = info->next;
		}
	}

	append_str(&buf, &len, "error: %s", T->msg);
	new_str(buf);

	return buf;
}

tug_Task* tug_task(const char* src, const char* code, char* errmsg) {
	Bytecode* bc = gen_bc(src, code, errmsg);
	if (!bc) return NULL;

	tug_Task* task = task_create(src, bc);
	return task;
}

void tug_resume(tug_Task* T) {
	task_resume(T);
}

void tug_pause(tug_Task* T) {
	T->state = TASK_YIELD;
}

tug_TaskState tug_getstate(tug_Task* T) {
	return T->state;
}

void tug_init(void) {
	#ifdef __ANDROID__

	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	seed_id = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;

	#elif defined(__linux__)

	struct timeval tv;
	gettimeofday(&tv, NULL);
	seed_id = (uint64_t)tv.tv_sec * 1000000000ULL + (uint64_t)tv.tv_usec * 1000ULL;

	#endif
	
	compiler_init();
	gc_init();
}

void tug_close(void) {
	vec_clearpool();
	for (size_t i = 0; i < varmap_poolc; i++) {
		VarMap* map = varmap_pool[i];
		gc_free(map->buckets);
		gc_free(map);
	}
	for (size_t i = 0; i < varmapentry_poolc; i++) {
		gc_free(varmapentry_pool[i]);
	}
	for (size_t i = 0; i < obj_poolc; i++) {
		gc_free(obj_pool[i]);
	}
	gc_close();
}
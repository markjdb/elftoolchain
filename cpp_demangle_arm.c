/*-
 * Copyright (c) 2008 Hyogeol Lee <hyogeollee@gmail.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer
 *    in this position and unchanged.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "vector_str.h"
#include "cpp_demangle_arm.h"

enum encode_type {
	ENCODE_FUNC, ENCODE_OP, ENCODE_OP_CT, ENCODE_OP_DT
};

struct demangle_data {
	bool	ptr, ref, cnst;
	const char *p;
	enum encode_type type;
	struct vector_str vec;
	struct vector_str arg;
};

#define SIMPLE_HASH(x,y)	(64 * x + y)
#define	CPP_DEMANGLE_ARM_TRY	128

static void	dest_demangle_data(struct demangle_data *);
static bool	init_demangle_data(struct demangle_data *);
static bool	push_CTDT(const char *, size_t, struct vector_str *);
static bool	read_class(struct demangle_data *);
static bool	read_func(struct demangle_data *);
static bool	read_func_name(struct demangle_data *);
static bool	read_op(struct demangle_data *);
static bool	read_qual_name(struct demangle_data *);
static bool	read_type(struct demangle_data *);

/*
 * Decode ARM style mangling.
 *
 * Return new allocated string or NULL.
 *
 * TODO.
 *  1. User type operator(__op).
 *  2. Type declarators : array(A), pointer to member(M).
 */
char *
cpp_demangle_ARM(const char *org)
{
	struct demangle_data d;
	size_t arg_begin, arg_len;
	unsigned int try;
	char *rtn, *arg;

	if (org == NULL)
		return (NULL);

	if (init_demangle_data(&d) == false)
		return (NULL);

	try = 0;
	rtn = NULL;

	d.p = org;
	if (read_func_name(&d) == false)
		goto clean;

	if (d.type == ENCODE_OP_CT) {
		if (push_CTDT("::", 2, &d.vec) == false)
			goto clean;

		goto flat;
	}

	if (d.type == ENCODE_OP_DT) {
		if (push_CTDT("::~", 3, &d.vec) == false)
			goto clean;

		goto flat;
	}

	/* function type */
	if (*d.p != 'F')
		goto clean;
	++d.p;

	/* start argument types */
	if (vector_str_push(&d.vec, "(", 1) == false)
		goto clean;

	for (;;) {
		if (*d.p == 'T') {
			size_t idx;
			char *str;

			idx = strtol(d.p + 1, &str, 10);
			if (idx == 0 && (errno == EINVAL || errno == ERANGE))
				goto clean;

			assert(idx > 0);
			assert(str != NULL);

			d.p = str;

			if (vector_str_push(&d.vec, d.arg.container[idx - 1],
				strlen(d.arg.container[idx - 1])) == false)
				goto clean;

			if (vector_str_push(&d.arg, d.arg.container[idx - 1],
				strlen(d.arg.container[idx - 1])) == false)
				goto clean;

			if (*d.p == '\0')
				break;

			continue;
		}

		if (*d.p == 'N') {
			size_t idx;
			char repeat;
			char *str;

			++d.p;
			assert(*d.p > 48 && *d.p < 58 && "*d.p not in ASCII numeric range");

			repeat = *d.p - 48;

			assert(repeat > 1);

			++d.p;

			idx = strtol(d.p, &str, 10);
			if (idx == 0 && (errno == EINVAL || errno == ERANGE))
				goto clean;

			assert(idx > 0);
			assert(str != NULL);

			d.p = str;

			for (int i = 0; i < repeat ; ++i) {
				if (vector_str_push(&d.vec, d.arg.container[idx - 1],
					strlen(d.arg.container[idx - 1])) == false)
					goto clean;

				if (vector_str_push(&d.arg, d.arg.container[idx - 1],
					strlen(d.arg.container[idx - 1])) == false)
					goto clean;

				
				if (i != repeat - 1 && vector_str_push(&d.vec, ", ", 2) == false)
					goto clean;
			}
			
			if (*d.p == '\0')
				break;

			continue;
		}

		arg_begin = d.vec.size;

		if (read_type(&d) == false)
			goto clean;

		if (d.ptr == true) {
			if (vector_str_push(&d.vec, "*", 1) == false)
				goto clean;

			d.ptr = false;
		}

		if (d.ref == true) {
			if (vector_str_push(&d.vec, "&", 1) == false)
				goto clean;

			d.ref = false;
		}

		if (d.cnst == true) {
			if (vector_str_push(&d.vec, " const", 6) == false)
				goto clean;

			d.cnst = false;
		}

		if (*d.p == '\0')
			break;

		if ((arg = vector_str_substr(&d.vec, arg_begin, d.vec.size - 1,
			    &arg_len)) == NULL)
			goto clean;

		if (vector_str_push(&d.arg, arg, strlen(arg)) == false)
			goto clean;

		free(arg);

		if (vector_str_push(&d.vec, ", ", 2) == false)
			goto clean;

		if (++try > CPP_DEMANGLE_ARM_TRY)
			goto clean;
	}

	/* end argument types */
	if (vector_str_push(&d.vec, ")", 1) == false)
		goto clean;

flat:
	rtn = vector_str_get_flat(&d.vec, NULL);
clean:
	dest_demangle_data(&d);

	return (rtn);
}

/* Test 'org' is encoded ARM style. */
bool
is_cpp_mangled_ARM(const char *org)
{
	size_t len;

	if (org == NULL)
		return (false);

	len = strlen(org);

	return (strnstr(org, "__", len) != NULL);
}

static void
dest_demangle_data(struct demangle_data *d)
{

	if (d != NULL) {
		vector_str_dest(&d->arg);
		vector_str_dest(&d->vec);
	}
}

static bool
init_demangle_data(struct demangle_data *d)
{

	if (d == NULL)
		return (false);

	d->ptr = false;
	d->ref = false;
	d->cnst = false;
	d->type = ENCODE_FUNC;

	return (vector_str_init(&d->vec) &&
	    vector_str_init(&d->arg));
}

static bool
push_CTDT(const char *s, size_t l, struct vector_str *v)
{

	if (s == NULL || v == NULL)
		return (false);

	if (vector_str_push(v, s, l) == false)
		return (false);
		
	assert(v->size > 1);
	if (vector_str_push(v, v->container[v->size - 2],
		strlen(v->container[v->size - 2])) == false)
		return (false);

	if (vector_str_push(v, "()", 2) == false)
		return (false);

	return (true);
}

static bool
read_class(struct demangle_data *d)
{
	size_t len;
	char *str;

	if (d == NULL)
		return (false);

	len = strtol(d->p, &str, 10);
	if (len == 0 && (errno == EINVAL || errno == ERANGE))
		return (false);

	assert(len > 0);
	assert(str != NULL);

	if (vector_str_push(&d->vec, str, len) == false)
		return (false);

	d->p = str + len;

	return (true);
}

static bool
read_func(struct demangle_data *d)
{
	size_t org_len, len;
	const char *name;
	char *delim;

	if (d == NULL)
		return (false);

	assert(d->p != NULL && "d->p (org str) is NULL");
	org_len = strlen(d->p);
	if ((delim = strnstr(d->p, "__", org_len)) == NULL)
		return (false);

	len = delim - d->p;
	assert(len != 0);

	name = d->p;

	d->p = delim + 2;

	if (*d->p == 'Q' && isdigit(*(d->p + 1))) {
		++d->p;

		if (read_qual_name(d) == false)
			return (false);
	} else if (isdigit(*d->p)) {
		if (read_class(d) == false)
			return (false);

		if (vector_str_push(&d->vec, "::", 2) == false)
			return (false);
	}

	if (vector_str_push(&d->vec, name, len) == false)
		return (false);

	return (true);
}

static bool
read_func_name(struct demangle_data *d)
{
	size_t len;
	bool rtn;
	char *op_name;

	if (d == NULL)
		return (false);

	rtn = false;
	op_name = NULL;

	assert(d->p != NULL && "d->p (org str) is NULL");

	if (*d->p == '_' && *(d->p + 1) == '_') {
		d->p += 2;
		
		d->type = ENCODE_OP;
		if (read_op(d) == false)
			return (false);

		if (d->type == ENCODE_OP_CT || d->type == ENCODE_OP_DT)
			return (true);

		/* skip "__" */
		d->p += 2;

		/* assume delimiter is removed */
		if (*d->p == 'Q' && isdigit(*(d->p + 1))) {
			++d->p;

			assert(d->vec.size > 0);

			len = strlen(d->vec.container[d->vec.size - 1]);
			if ((op_name = malloc(sizeof(char) * (len + 1)))
			    == NULL)
				return (false);

			snprintf(op_name, len + 1, "%s",
			    d->vec.container[d->vec.size - 1]);
			vector_str_pop(&d->vec);

			if (read_qual_name(d) == false)
				goto clean;

			if (vector_str_push(&d->vec, "::", 2) == false)
				goto clean;

			if (vector_str_push(&d->vec, op_name, len) == false)
				goto clean;

			rtn = true;
		} else if (isdigit(*d->p)) {
			assert(d->vec.size > 0);

			len = strlen(d->vec.container[d->vec.size - 1]) + 1;
			if ((op_name = malloc(sizeof(char) * (len + 1)))
			    == NULL)
				return (false);

			snprintf(op_name, len + 1, "%s",
			    d->vec.container[d->vec.size - 1]);
			vector_str_pop(&d->vec);

			if (read_class(d) == false)
				goto clean;

			if (vector_str_push(&d->vec, "::", 2) == false)
				goto clean;

			if (vector_str_push(&d->vec, op_name, len) == false)
				goto clean;

			rtn = true;
		}
	} else
		return (read_func(d));

clean:
	free(op_name);

	return (rtn);
}

static bool
read_op(struct demangle_data *d)
{

	if (d == NULL)
		return (false);

	assert(d->p != NULL && "d->p (org str) is NULL");

	switch (SIMPLE_HASH(*(d->p), *(d->p+1))) {
	case SIMPLE_HASH('m', 'l') :
		d->p += 2;
		return (vector_str_push(&d->vec, "operator*", 9));
	case SIMPLE_HASH('d', 'v') :
		d->p += 2;
		return (vector_str_push(&d->vec, "operator/", 9));
	case SIMPLE_HASH('m', 'd') :
		d->p += 2;
		return (vector_str_push(&d->vec, "operator%", 9));
	case SIMPLE_HASH('p', 'l') :
		d->p += 2;
		return (vector_str_push(&d->vec, "operator+", 9));
	case SIMPLE_HASH('m', 'i') :
		d->p += 2;
		return (vector_str_push(&d->vec, "operator-", 9));
	case SIMPLE_HASH('l', 's') :
		d->p += 2;
		return (vector_str_push(&d->vec, "operator<<", 10));
	case SIMPLE_HASH('r', 's') :
		d->p += 2;
		return (vector_str_push(&d->vec, "operator>>", 10));
	case SIMPLE_HASH('e', 'q') :
		d->p += 2;
		return (vector_str_push(&d->vec, "operator==", 10));
	case SIMPLE_HASH('n', 'e') :
		d->p += 2;
		return (vector_str_push(&d->vec, "operator!=", 10));
	case SIMPLE_HASH('l', 't') :
		d->p += 2;
		return (vector_str_push(&d->vec, "operator<", 9));
	case SIMPLE_HASH('g', 't') :
		d->p += 2;
		return (vector_str_push(&d->vec, "operator>", 9));
	case SIMPLE_HASH('l', 'e') :
		d->p += 2;
		return (vector_str_push(&d->vec, "operator<=", 10));
	case SIMPLE_HASH('g', 'e') :
		d->p += 2;
		return (vector_str_push(&d->vec, "operator>=", 10));
	case SIMPLE_HASH('a', 'd') :
		d->p += 2;
		if (*d->p == 'v') {
			++d->p;
			return (vector_str_push(&d->vec, "operator/=",
				10));
		} else
			return (vector_str_push(&d->vec, "operator&", 9));
	case SIMPLE_HASH('o', 'r') :
		d->p += 2;
		return (vector_str_push(&d->vec, "operator|", 9));
	case SIMPLE_HASH('e', 'r') :
		d->p += 2;
		return (vector_str_push(&d->vec, "operator^", 9));
	case SIMPLE_HASH('a', 'a') :
		d->p += 2;
		if (*d->p == 'd') {
			++d->p;
			return (vector_str_push(&d->vec, "operator&=",
				10));
		} else
			return (vector_str_push(&d->vec, "operator&&",
				10));
	case SIMPLE_HASH('o', 'o') :
		d->p += 2;
		return (vector_str_push(&d->vec, "operator||", 10));
	case SIMPLE_HASH('n', 't') :
		d->p += 2;
		return (vector_str_push(&d->vec, "operator!", 9));
	case SIMPLE_HASH('c', 'o') :
		d->p += 2;
		return (vector_str_push(&d->vec, "operator~", 9));
	case SIMPLE_HASH('p', 'p') :
		d->p += 2;
		return (vector_str_push(&d->vec, "operator++", 10));
	case SIMPLE_HASH('m', 'm') :
		d->p += 2;
		return (vector_str_push(&d->vec, "operator--", 10));
	case SIMPLE_HASH('a', 's') :
		d->p += 2;
		return (vector_str_push(&d->vec, "operator=", 9));
	case SIMPLE_HASH('r', 'f') :
		d->p += 2;
		return (vector_str_push(&d->vec, "operator->", 10));
	case SIMPLE_HASH('a', 'p') :
		/* apl */
		assert(*(d->p + 2) == 'l');

		d->p += 3;
		return (vector_str_push(&d->vec, "operator+=", 10));
	case SIMPLE_HASH('a', 'm') :
		d->p += 2;
		if (*d->p == 'i') {
			++d->p;
			return (vector_str_push(&d->vec, "operator-=",
				10));
		} else if (*d->p == 'u') {
			++d->p;
			return (vector_str_push(&d->vec, "operator*=",
				10));
		} else if (*d->p == 'd') {
			++d->p;
			return (vector_str_push(&d->vec, "operator%=",
				10));
		}

		return (false);
	case SIMPLE_HASH('a', 'l') :
		/* als */
		assert(*(d->p + 2) == 's');

		d->p += 3;
		return (vector_str_push(&d->vec, "operator<<=", 11));
	case SIMPLE_HASH('a', 'r') :
		/* ars */
		assert(*(d->p + 2) == 's');

		d->p += 3;
		return (vector_str_push(&d->vec, "operator>>=", 11));
	case SIMPLE_HASH('a', 'o') :
		/* aor */
		assert(*(d->p + 2) == 'r');

		d->p += 3;
		return (vector_str_push(&d->vec, "operator|=", 10));
	case SIMPLE_HASH('a', 'e') :
		/* aer */
		assert(*(d->p + 2) == 'r');

		d->p += 3;
		return (vector_str_push(&d->vec, "operator^=", 10));
	case SIMPLE_HASH('c', 'm') :
		d->p += 2;
		return (vector_str_push(&d->vec, "operator,", 9));
	case SIMPLE_HASH('r', 'm') :
		d->p += 2;
		return (vector_str_push(&d->vec, "operator->*", 11));
	case SIMPLE_HASH('c', 'l') :
		d->p += 2;
		return (vector_str_push(&d->vec, "()", 2));
	case SIMPLE_HASH('v', 'c') :
		d->p += 2;
		return (vector_str_push(&d->vec, "[]", 2));
	case SIMPLE_HASH('c', 't') :
		d->p += 4;
		d->type = ENCODE_OP_CT;

		if (*d->p == 'Q' && isdigit(*(d->p + 1))) {
			++d->p;

			return (read_qual_name(d));
		} else if (isdigit(*d->p))
			return (read_class(d));

		return (false);
	case SIMPLE_HASH('d', 't') :
		d->p += 4;
		d->type = ENCODE_OP_DT;

		if (*d->p == 'Q' && isdigit(*(d->p + 1))) {
			++d->p;

			return (read_qual_name(d));
		} else if (isdigit(*d->p))
			return (read_class(d));

		return (false);
	case SIMPLE_HASH('n', 'w') :
		d->p += 2;
		return (vector_str_push(&d->vec, "operator new()", 14));
	case SIMPLE_HASH('d', 'l') :
		d->p += 2;
		return (vector_str_push(&d->vec, "operator delete()",
			17));
	case SIMPLE_HASH('o', 'p') :
		/* operator xx */
	default :
		return (false);
	};
}

/* single digit + class names */
static bool
read_qual_name(struct demangle_data *d)
{
	char num;

	if (d == NULL)
		return (false);

	assert(d->p != NULL && "d->p (org str) is NULL");
	assert(*d->p > 48 && *d->p < 58 && "*d->p not in ASCII numeric range");

	num = *d->p - 48;

	assert(num > 0);

	++d->p;
	for (int i = 0; i < num ; ++i) {
		if (read_class(d) == false)
			return (false);

		if (vector_str_push(&d->vec, "::", 2) == false)
			return (false);
	}

	d->p = d->p + 2;

	return (true);
}

static bool
read_type(struct demangle_data *d)
{

	if (d == NULL)
		return (false);

	assert(d->p != NULL && "d->p (org str) is NULL");

	while (*d->p == 'U' || *d->p == 'C' || *d->p == 'V' || *d->p == 'S' ||
	       *d->p == 'P' || *d->p == 'R' || *d->p == 'A' || *d->p == 'F' ||
	       *d->p == 'M') {
		switch (*d->p) {
		case 'U' :
			++d->p;

			if (vector_str_push(&d->vec, "unsigned ", 9) == false)
				return (false);

			break;
		case 'C' :
			++d->p;

			if (*d->p == 'P')
				d->cnst = true;
			else {
				if (vector_str_push(&d->vec, "const ", 6) ==
				    false)
					return (false);
			} 

			break;
		case 'V' :
			++d->p;

			if (vector_str_push(&d->vec, "volatile ", 9) == false)
				return (false);

			break;
		case 'S' :
			++d->p;
		
			if (vector_str_push(&d->vec, "signed ", 7) == false)
				return (false);

			break;
		case 'P' :
			++d->p;

			/* function ptr */
			if (*d->p == 'F') {
				struct demangle_data fptr;
				char *arg_type, *rtn_type;
				int lim;

				if (init_demangle_data(&fptr) == false)
					return (false);

				fptr.p = d->p + 1;
				lim = 0;
				arg_type = NULL;
				rtn_type = NULL;

				for (;;) {
					if (read_type(&fptr) == false) {
						dest_demangle_data(&fptr);

						return (false);
					}

					if (fptr.ptr == true) {
						if (vector_str_push(&fptr.vec,
							"*", 1) == false) {
							dest_demangle_data(&fptr);

							return (false);
						}

						fptr.ptr = false;
					}

					if (fptr.ref == true) {
						if (vector_str_push(&fptr.vec,
							"&", 1) == false) {
							dest_demangle_data(&fptr);

							return (false);
						}

						fptr.ref = false;
					}

					if (fptr.cnst == true) {
						if (vector_str_push(&fptr.vec,
							" const", 6) == false) {
							dest_demangle_data(&fptr);

							return (false);
						}

						fptr.cnst = false;
					}

					if (*fptr.p == '_')
						break;

					if (vector_str_push(&fptr.vec, ", ",
						2) == false) {
							dest_demangle_data(&fptr);

							return (false);
					}

					if (++lim > CPP_DEMANGLE_ARM_TRY) {

						dest_demangle_data(&fptr);

						return (false);
					}
				}

				arg_type = vector_str_get_flat(&fptr.vec, NULL);
				/* skip '_' */
				d->p = fptr.p + 1;

				dest_demangle_data(&fptr);

				if (init_demangle_data(&fptr) == false) {
					free(arg_type);

					return (false);
				}

				fptr.p = d->p;
				lim = 0;

				if (read_type(&fptr) == false) {
					free(arg_type);
					dest_demangle_data(&fptr);

					return (false);
				}

				rtn_type = vector_str_get_flat(&fptr.vec, NULL);
				d->p = fptr.p;


				dest_demangle_data(&fptr);

				if (vector_str_push(&d->vec, rtn_type, 
					strlen(rtn_type)) == false) {
					free(rtn_type);
					free(arg_type);

					return (false);
				}

				free(rtn_type);

				if (vector_str_push(&d->vec, " (*)(", 5)
				    == false) {
					free(arg_type);

					return (false);
				}

				if (vector_str_push(&d->vec, arg_type,
					strlen(arg_type)) == false) {
					free(arg_type);

					return (false);
				}

				free(arg_type);

				return (vector_str_push(&d->vec, ")", 1));
			} else
				d->ptr = true;

			break;
		case 'R' :
			++d->p;

			d->ref = true;

			break;
		case 'A' :
		case 'F' :
		case 'M' :
		default :
			break;
		};
	};

	if (isdigit(*d->p))
		return (read_class(d));

	switch (*d->p) {
	case 'Q' :
		return (read_qual_name(d));
	case 'v' :
		++d->p;

		return (vector_str_push(&d->vec, "void", 4));
	case 'c' :
		++d->p;
		
		return (vector_str_push(&d->vec, "char", 4));
	case 's' :
		++d->p;
		
		return (vector_str_push(&d->vec, "short", 5));
	case 'i' :
		++d->p;

		return (vector_str_push(&d->vec, "int", 3));
	case 'l' :
		++d->p;

		return (vector_str_push(&d->vec, "long", 4));
	case 'f' :
		++d->p;

		return (vector_str_push(&d->vec, "float", 5));
	case 'd':
		++d->p;

		return (vector_str_push(&d->vec, "double", 6));
	case 'r':
		++d->p;

		return (vector_str_push(&d->vec, "long double", 11));
	case 'e':
		++d->p;

		return (vector_str_push(&d->vec, "...", 3));
	default:
		return (false);
	};

	/* NOTREACHED */
	return (false);
}

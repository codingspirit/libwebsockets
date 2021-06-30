/*
 * libwebsockets - small server side websockets and web server implementation
 *
 * Copyright (C) 2010 - 2021 Andy Green <andy@warmcat.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Stream parser for RFC8949 CBOR
 */

#include "private-lib-core.h"
#include <string.h>
#include <stdio.h>

#define lwsl_lecp lwsl_debug

static const char * const parser_errs[] = {
	"",
	"",
	"Bad CBOR coding",
	"Unknown",
	"Parser callback errored (see earlier error)",
	"Overflow"
};

enum lecp_states {
	LECP_OPC,
	LECP_COLLECT,
	LECP_SIMPLEX8,
	LECP_COLLATE,
	LECP_ONLY_SAME
};

void
lecp_construct(struct lecp_ctx *ctx, lecp_callback cb, void *user,
	       const char * const *paths, unsigned char count_paths)
{
	uint16_t x = 0x1234;

	memset(ctx, 0, sizeof(*ctx) - sizeof(ctx->buf));

	ctx->user		= user;
	ctx->pst[0].cb		= cb;
	ctx->pst[0].paths	= paths;
	ctx->pst[0].count_paths = count_paths;
	ctx->be			= *((uint8_t *)&x) == 0x12;

	ctx->st[0].s		= LECP_OPC;

	ctx->pst[0].cb(ctx, LECPCB_CONSTRUCTED);
}

void
lecp_destruct(struct lecp_ctx *ctx)
{
	/* no allocations... just let callback know what it happening */
	if (ctx->pst[0].cb)
		ctx->pst[0].cb(ctx, LECPCB_DESTRUCTED);
}

void
lecp_change_callback(struct lecp_ctx *ctx, lecp_callback cb)
{
	ctx->pst[0].cb(ctx, LECPCB_DESTRUCTED);
	ctx->pst[0].cb = cb;
	ctx->pst[0].cb(ctx, LECPCB_CONSTRUCTED);
}


const char *
lecp_error_to_string(int e)
{
	if (e > 0)
		e = 0;
	else
		e = -e;

	if (e >= (int)LWS_ARRAY_SIZE(parser_errs))
		return "Unknown error";

	return parser_errs[e];
}

static void
ex(struct lecp_ctx *ctx, void *_start, size_t len)
{
	struct _lecp_stack *st = &ctx->st[ctx->sp];
	uint8_t *start = (uint8_t *)_start;

	st->s = LECP_COLLECT;
	st->collect_rem = (uint8_t)len;

	if (ctx->be)
		ctx->collect_tgt = start;
	else
		ctx->collect_tgt = start + len - 1;
}

static void
lecp_check_path_match(struct lecp_ctx *ctx)
{
	const char *p, *q;
	size_t s = sizeof(char *);
	int n;

	if (ctx->path_stride)
		s = ctx->path_stride;

	/* we only need to check if a match is not active */
	for (n = 0; !ctx->path_match &&
	     n < ctx->pst[ctx->pst_sp].count_paths; n++) {
		ctx->wildcount = 0;
		p = ctx->path;

		q = *((char **)(((char *)ctx->pst[ctx->pst_sp].paths) +
							((unsigned int)n * s)));

		while (*p && *q) {
			if (*q != '*') {
				if (*p != *q)
					break;
				p++;
				q++;
				continue;
			}
			ctx->wild[ctx->wildcount++] =
				    (uint16_t)lws_ptr_diff_size_t(p, ctx->path);
			q++;
			/*
			 * if * has something after it, match to .
			 * if ends with *, eat everything.
			 * This implies match sequences must be ordered like
			 *  x.*.*
			 *  x.*
			 * if both options are possible
			 */
			while (*p && (*p != '.' || !*q))
				p++;
		}
		if (*p || *q)
			continue;

		ctx->path_match = (uint8_t)(n + 1);
		ctx->path_match_len = ctx->pst[ctx->pst_sp].ppos;
		return;
	}

	if (!ctx->path_match)
		ctx->wildcount = 0;
}

int
lecp_push(struct lecp_ctx *ctx, char s_start, char s_end, char state)
{
	struct _lecp_stack *st = &ctx->st[ctx->sp];

	if (ctx->sp + 1 == LWS_ARRAY_SIZE(ctx->st))
		return LECP_STACK_OVERFLOW;

	if (s_start && ctx->pst[ctx->pst_sp].cb(ctx, s_start))
		return LECP_REJECT_CALLBACK;

	lwsl_lecp("%s: pushing from sp %d, parent "
		  "(opc %d, indet %d, collect_rem %d)\n",
		  __func__, ctx->sp, st->opcode >> 5, st->indet,
		  (int)st->collect_rem);


	st->pop_iss = s_end; /* issue this when we pop back here */
	ctx->st[ctx->sp + 1] = *st;
	ctx->sp++;
	st++;

	st->s			= state;
	st->collect_rem		= 0;
	st->intermediate	= 0;
	st->indet		= 0;
	st->ordinal		= 0;

	return 0;
}

int
lecp_pop(struct lecp_ctx *ctx)
{
	struct _lecp_stack *st;

	assert(ctx->sp);
	ctx->sp--;

	st = &ctx->st[ctx->sp];

	if (st->pop_iss == LECPCB_ARRAY_END) {
		assert(ctx->ipos);
		ctx->ipos--;
	}

	ctx->pst[ctx->pst_sp].ppos = st->p;
	ctx->path[st->p] = '\0';
	lecp_check_path_match(ctx);

	lwsl_lecp("%s: popping to sp %d, parent "
		  "(opc %d, indet %d, collect_rem %d)\n",
		   __func__, ctx->sp, st->opcode >> 5, st->indet,
		   (int)st->collect_rem);

	if (st->pop_iss && ctx->pst[ctx->pst_sp].cb(ctx, st->pop_iss))
		return LECP_REJECT_CALLBACK;

	return 0;
}

static struct _lecp_stack *
lwcp_st_parent(struct lecp_ctx *ctx)
{
	assert(ctx->sp);

	return &ctx->st[ctx->sp - 1];
}

int
lwcp_completed(struct lecp_ctx *ctx, char indet)
{
	int r, il = ctx->ipos;

	ctx->st[ctx->sp].s = LECP_OPC;

	while (ctx->sp) {
		struct _lecp_stack *parent = lwcp_st_parent(ctx);

		lwsl_lecp("%s: sp %d, parent "
			  "(opc %d, indet %d, collect_rem %d)\n",
			  __func__, ctx->sp, parent->opcode >> 5, parent->indet,
			  (int)parent->collect_rem);

		parent->ordinal++;
		if (parent->opcode == LWS_CBOR_MAJTYP_ARRAY) {
			assert(il);
			il--;
			ctx->i[il]++;
		}

		if (!indet && parent->indet) {
			lwsl_lecp("%s: abandoning walk as parent needs indet\n", __func__);
			break;
		}

		if (!parent->indet && parent->collect_rem) {
			parent->collect_rem--;
			lwsl_lecp("%s: sp %d, parent (opc %d, indet %d, collect_rem -> %d)\n",
					__func__, ctx->sp, parent->opcode >> 5, parent->indet, (int)parent->collect_rem);

			if (parent->collect_rem)
				break;
		}

		lwsl_lecp("%s: parent (opc %d) collect_rem became zero\n", __func__, parent->opcode >> 5);

		ctx->st[ctx->sp - 1].s = LECP_OPC;
		r = lecp_pop(ctx);
		if (r)
			return r;
		indet = 0;
	}

	return 0;
}

static int
lwcp_is_indet_string(struct lecp_ctx *ctx)
{
	if (ctx->st[ctx->sp].indet)
		return 1;

	if (!ctx->sp)
		return 0;

	if (lwcp_st_parent(ctx)->opcode != LWS_CBOR_MAJTYP_BSTR &&
	    lwcp_st_parent(ctx)->opcode != LWS_CBOR_MAJTYP_TSTR)
		return 0;

	if (ctx->st[ctx->sp - 1].indet)
		return 1;

	return 0;
}

int
lecp_parse(struct lecp_ctx *ctx, const uint8_t *cbor, size_t len)
{
	int ret;

	while (len--) {
		struct _lecp_parsing_stack *pst = &ctx->pst[ctx->pst_sp];
		struct _lecp_stack *st = &ctx->st[ctx->sp];
		uint8_t c, sm, o;
		char to;

		c = *cbor++;

		switch (st->s) {
		/*
		 * We're getting the nex opcode
		 */
		case LECP_OPC:
			st->opcode = ctx->item.opcode = c & LWS_CBOR_MAJTYP_MASK;
			sm = c & LWS_CBOR_SUBMASK;
			to = 0;

			lwsl_lecp("%s: %d: OPC %d|%d\n", __func__, ctx->sp,
					c >> 5, sm);

			switch (st->opcode) {
			case LWS_CBOR_MAJTYP_UINT:
				ctx->present = LECPCB_VAL_NUM_UINT;
				if (sm < LWS_CBOR_1) {
					ctx->item.u.i64 = (int64_t)sm;
					goto issue;
				}
				goto i2;

			case LWS_CBOR_MAJTYP_INT_NEG:
				ctx->present = LECPCB_VAL_NUM_INT;
				if (sm < 24) {
					ctx->item.u.i64 = (-1ll) - (int64_t)sm;
					goto issue;
				}
i2:
				if (sm >= LWS_CBOR_RESERVED)
					goto bad_coding;
				ctx->item.u.u64 = 0;
				o = (uint8_t)(1 << (sm - LWS_CBOR_1));
				ex(ctx, (uint8_t *)&ctx->item.u.u64, o);
				break;

			case LWS_CBOR_MAJTYP_BSTR:
				to = LECPCB_VAL_BLOB_END - LECPCB_VAL_STR_END;

				/* fallthru */

			case LWS_CBOR_MAJTYP_TSTR:
				/*
				 * The first thing is the string length, it's
				 * going to either be a byte count for the
				 * string or the indefinite length marker
				 * followed by determinite-length chunks of the
				 * same MAJTYP
				 */

				ctx->npos = 0;
				ctx->buf[0] = '\0';

				if ((!ctx->sp || (ctx->sp &&
				    !ctx->st[ctx->sp - 1].intermediate)) &&
				    pst->cb(ctx, (char)(LECPCB_VAL_STR_START + to)))
					goto reject_callback;

				if (!sm) {
					if (pst->cb(ctx, (char)(LECPCB_VAL_STR_END + to)))
						goto reject_callback;
					lwcp_completed(ctx, 0);
					break;
				}

				if (sm < LWS_CBOR_1) {
					st->indet = 0;
					st->collect_rem = sm;
					st->s = LECP_COLLATE;
					break;
				}

				if (sm < LWS_CBOR_RESERVED)
					goto i2;

				if (sm != LWS_CBOR_INDETERMINITE)
					goto bad_coding;

				st->indet = 1;

				st->p = pst->ppos;
				lecp_push(ctx, 0, (char)(LECPCB_VAL_STR_END + to),
						  LECP_ONLY_SAME);
				break;

			case LWS_CBOR_MAJTYP_ARRAY:
				ctx->npos = 0;
				ctx->buf[0] = '\0';

				if (pst->ppos + 3u >= sizeof(ctx->path))
					goto reject_overflow;

				st->p = pst->ppos;
				ctx->path[pst->ppos++] = '[';
				ctx->path[pst->ppos++] = ']';
				ctx->path[pst->ppos] = '\0';

				lecp_check_path_match(ctx);

				if (ctx->ipos + 1u >= LWS_ARRAY_SIZE(ctx->i))
					goto reject_overflow;

				ctx->i[ctx->ipos++] = 0;

				if (pst->cb(ctx, LECPCB_ARRAY_START))
					goto reject_callback;

				if (!sm) {
					if (pst->cb(ctx, LECPCB_ARRAY_END))
						goto reject_callback;
					pst->ppos = st->p;
					ctx->path[pst->ppos] = '\0';
					ctx->ipos--;
					lecp_check_path_match(ctx);
					lwcp_completed(ctx, 0);
					break;
				}
				if (sm < LWS_CBOR_1) {
					st->indet = 0;
					st->collect_rem = sm;
					goto push_a;
				}

				if (sm < LWS_CBOR_RESERVED)
					goto i2;

				if (sm != LWS_CBOR_INDETERMINITE)
					goto bad_coding;

				st->indet = 1;
push_a:
				lecp_push(ctx, 0, LECPCB_ARRAY_END, LECP_OPC);
				break;

			case LWS_CBOR_MAJTYP_MAP:
				ctx->npos = 0;
				ctx->buf[0] = '\0';

				if (pst->ppos + 1u >= sizeof(ctx->path))
					goto reject_overflow;

				st->p = pst->ppos;
				ctx->path[pst->ppos++] = '.';
				ctx->path[pst->ppos] = '\0';

				lecp_check_path_match(ctx);

				if (pst->cb(ctx, LECPCB_OBJECT_START))
					goto reject_callback;

				if (!sm) {
					if (pst->cb(ctx, LECPCB_OBJECT_END))
						goto reject_callback;
					pst->ppos = st->p;
					ctx->path[pst->ppos] = '\0';
					lecp_check_path_match(ctx);
					lwcp_completed(ctx, 0);
					break;
				}
				if (sm < LWS_CBOR_1) {
					st->indet = 0;
					st->collect_rem = (uint64_t)(sm * 2);
					goto push_m;
				}

				if (sm < LWS_CBOR_RESERVED)
					goto i2;

				if (sm != LWS_CBOR_INDETERMINITE)
					goto bad_coding;

				st->indet = 1;
push_m:
				lecp_push(ctx, 0, LECPCB_OBJECT_END, LECP_OPC);
				break;

			case LWS_CBOR_MAJTYP_TAG:
				/* tag has one or another kind of int first */
				if (sm < LWS_CBOR_1) {
					/*
					 * We have a literal tag number, push
					 * to decode the tag body
					 */
					ctx->item.u.u64 = st->tag = (uint64_t)sm;
					goto start_tag_enclosure;
				}
				/*
				 * We have to do more stuff to get the tag
				 * number...
				 */
				goto i2;

			case LWS_CBOR_MAJTYP_FLOAT:
				/*
				 * This can also be a bunch of specials as well
				 * as sizes of float...
				 */
				sm = c & LWS_CBOR_SUBMASK;

				switch (sm) {
				case LWS_CBOR_SWK_FALSE:
					ctx->present = LECPCB_VAL_FALSE;
					goto issue;

				case LWS_CBOR_SWK_TRUE:
					ctx->present = LECPCB_VAL_TRUE;
					goto issue;

				case LWS_CBOR_SWK_NULL:
					ctx->present = LECPCB_VAL_NULL;
					goto issue;

				case LWS_CBOR_SWK_UNDEFINED:
					ctx->present = LECPCB_VAL_UNDEFINED;
					goto issue;

				case LWS_CBOR_M7_SUBTYP_SIMPLE_X8:
					st->s = LECP_SIMPLEX8;
					break;

				case LWS_CBOR_M7_SUBTYP_FLOAT16:
					ctx->present = LECPCB_VAL_FLOAT16;
					ex(ctx, &ctx->item.u.hf, 2);
					break;

				case LWS_CBOR_M7_SUBTYP_FLOAT32:
					ctx->present = LECPCB_VAL_FLOAT32;
					ex(ctx, &ctx->item.u.f, 4);
					break;

				case LWS_CBOR_M7_SUBTYP_FLOAT64:
					ctx->present = LECPCB_VAL_FLOAT64;
					ex(ctx, &ctx->item.u.d, 8);
					break;

				case LWS_CBOR_M7_BREAK:
					if (!ctx->sp ||
					    !ctx->st[ctx->sp - 1].indet)
						goto bad_coding;

					lwcp_completed(ctx, 1);
					break;

				default:
					/* handle as simple */
					ctx->item.u.u64 = (uint64_t)sm;
					if (pst->cb(ctx, LECPCB_VAL_SIMPLE))
						goto reject_callback;
					break;
				}
				break;
			}
			break;

		/*
		 * We're collecting int / float pieces
		 */
		case LECP_COLLECT:
			if (ctx->be)
				*ctx->collect_tgt++ = c;
			else
				*ctx->collect_tgt-- = c;

			if (--st->collect_rem)
				break;

			/*
			 * We collected whatever it was...
			 */

			ctx->npos = 0;
			ctx->buf[0] = '\0';

			switch (st->opcode) {
			case LWS_CBOR_MAJTYP_BSTR:
			case LWS_CBOR_MAJTYP_TSTR:
				st->collect_rem = ctx->item.u.u64;
				st->s = LECP_COLLATE;
				break;

			case LWS_CBOR_MAJTYP_ARRAY:
				st->collect_rem = ctx->item.u.u64;
				lecp_push(ctx, 0, LECPCB_ARRAY_END, LECP_OPC);
				break;

			case LWS_CBOR_MAJTYP_MAP:
				st->collect_rem = ctx->item.u.u64 * 2;
				lecp_push(ctx, 0, LECPCB_OBJECT_END, LECP_OPC);
				break;

			case LWS_CBOR_MAJTYP_TAG:
				st->tag = ctx->item.u.u64;
				goto start_tag_enclosure;

			default:
				/*
				 * ... then issue what we collected as a
				 * literal
				 */

				if (st->opcode == LWS_CBOR_MAJTYP_INT_NEG)
					ctx->item.u.i64 = (-1ll) - ctx->item.u.i64;

				goto issue;
			}
			break;

		case LECP_SIMPLEX8:
			/*
			 * Extended SIMPLE byte for 7|24 opcode, no uses
			 * for it in RFC8949
			 */
			if (c <= LWS_CBOR_INDETERMINITE)
				/*
				 * Duplication of implicit simple values is
				 * denied by RFC8949 3.3
				 */
				goto bad_coding;

			ctx->item.u.u64 = (uint64_t)c;
			if (pst->cb(ctx, LECPCB_VAL_SIMPLE))
				goto reject_callback;

			lwcp_completed(ctx, 0);
			break;

		case LECP_COLLATE:
			/*
			 * let's grab b/t string content into the context
			 * buffer, and issue chunks from there
			 */

			ctx->buf[ctx->npos++] = (char)c;
			if (st->collect_rem)
				st->collect_rem--;

			/* spill at chunk boundaries, or if we filled the buf */
			if (ctx->npos != sizeof(ctx->buf) - 1 &&
			    st->collect_rem)
				break;

			/* spill */
			ctx->buf[ctx->npos] = '\0';

			/* if it's a map name, deal with the path */
			if (ctx->sp &&
			    lwcp_st_parent(ctx)->opcode == LWS_CBOR_MAJTYP_MAP &&
			    !(lwcp_st_parent(ctx)->ordinal & 1)) {
				if (lwcp_st_parent(ctx)->ordinal)
					pst->ppos = st->p;
				st->p = pst->ppos;
				if (pst->ppos + ctx->npos > sizeof(ctx->path))
					goto reject_overflow;
				memcpy(&ctx->path[pst->ppos], ctx->buf,
				       (size_t)(ctx->npos + 1));
				pst->ppos = (uint8_t)(pst->ppos + ctx->npos);
				lecp_check_path_match(ctx);
			}

			to = 0;
			if (ctx->item.opcode == LWS_CBOR_MAJTYP_BSTR)
				to = LECPCB_VAL_BLOB_END - LECPCB_VAL_STR_END;

			o = (uint8_t)(LECPCB_VAL_STR_END + to);
			c = (st->collect_rem /* more to come at this layer */ ||
			    /* we or direct parent is indeterminite */
			    lwcp_is_indet_string(ctx));

			if (ctx->sp)
				ctx->st[ctx->sp - 1].intermediate = !!c;
			if (c)
				o--;

			if (pst->cb(ctx, (char)o))
				goto reject_callback;
			ctx->npos = 0;
			ctx->buf[0] = '\0';
			st->s = LECP_OPC;
			if (o == LECPCB_VAL_STR_END + to)
				lwcp_completed(ctx, 0);

			break;

		case LECP_ONLY_SAME:
			/*
			 * deterministic sized chunks same MAJTYP as parent
			 * level only (BSTR and TSTR frags inside interderminite
			 * BSTR or TSTR)
			 *
			 * Clean end when we see M7|31
			 */
			if (!ctx->sp) {
				/*
				 * We should only come here by pushing on stack
				 */
				assert(0);
				return LECP_STACK_OVERFLOW;
			}

			if (c == (LWS_CBOR_MAJTYP_FLOAT | LWS_CBOR_M7_BREAK)) {
				/* if's the end of an interdetminite list */
				if (!ctx->sp || !ctx->st[ctx->sp - 1].indet)
					/*
					 * Can't have a break without an
					 * indeterminite parent
					 */
					goto bad_coding;

				if (lwcp_completed(ctx, 1))
					goto reject_callback;
				break;
			}

			if (st->opcode != lwcp_st_parent(ctx)->opcode)
				/*
				 * Fragments have to be of the same type as the
				 * outer opcode
				 */
				goto bad_coding;

			sm = c & LWS_CBOR_SUBMASK;

			if (sm == LWS_CBOR_INDETERMINITE)
				/* indeterminite length frags not allowed */
				goto bad_coding;

			if (sm < LWS_CBOR_1) {
				st->indet = 0;
				st->collect_rem = (uint64_t)sm;
				st->s = LECP_COLLATE;
				break;
			}

			if (sm >= LWS_CBOR_RESERVED)
				goto bad_coding;

			goto i2;

		default:
			assert(0);
			return -1;
		}

		continue;

start_tag_enclosure:
		st->p = pst->ppos;
		ret = lecp_push(ctx, LECPCB_TAG_START, LECPCB_TAG_END, LECP_OPC);
		if (ret)
			return ret;

		continue;

issue:
		if (ctx->item.opcode == LWS_CBOR_MAJTYP_TAG) {
			st->tag = ctx->item.u.u64;
			goto start_tag_enclosure;
		}

		/* we are just a number */

		if (pst->cb(ctx, ctx->present))
			goto reject_callback;

		lwcp_completed(ctx, 0);

	}

	if (!ctx->sp && ctx->st[0].s == LECP_OPC)
		return 0;

	return LECP_CONTINUE;

reject_overflow:
	ret = LECP_STACK_OVERFLOW;
	goto reject;

bad_coding:
	ret = LECP_REJECT_BAD_CODING;
	goto reject;

reject_callback:
	ret = LECP_REJECT_CALLBACK;

reject:
	ctx->pst[ctx->pst_sp].cb(ctx, LECPCB_FAILED);

	return ret;
}

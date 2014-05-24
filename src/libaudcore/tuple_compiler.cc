/*
 * tuple_compiler.c
 * Copyright (c) 2007 Matti 'ccr' Hämäläinen
 * Copyright (c) 2011-2013 John Lindgren
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions, and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions, and the following disclaimer in the documentation
 *    provided with the distribution.
 *
 * This software is provided "as is" and without any warranty, express or
 * implied. In no event shall the authors be liable for any damages arising from
 * the use of this software.
 */

#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <glib.h>

#include "audstrings.h"
#include "tuple_compiler.h"

#define MAX_STR     256
#define MAX_VARS    4

#define GET_VAR(c, i)  (& g_array_index ((c), TupleEvalVar, (i)))

#define tuple_error(ctx, ...) fprintf (stderr, "Tuple compiler: " __VA_ARGS__)

enum TupleEvalOp
{
    OP_RAW = 0,       /* plain text */
    OP_FIELD,         /* a field/variable */
    OP_EXISTS,
    OP_EQUALS,
    OP_NOT_EQUALS,
    OP_GT,
    OP_GTEQ,
    OP_LT,
    OP_LTEQ,
    OP_IS_EMPTY
};

enum TupleEvalType
{
    TUPLE_VAR_FIELD = 0,
    TUPLE_VAR_CONST
};

struct TupleEvalNode
{
    TupleEvalOp opcode;                          /* operator, see OP_ enums */
    int var[MAX_VARS];                           /* tuple variable references */
    char * text;                                 /* raw text, if any (OP_RAW) */
    TupleEvalNode * children, * next, * prev;    /* children of this struct, and pointer to next node. */
};

struct TupleEvalVar
{
    char * name;
    TupleEvalType type;      /* Type of variable, see VAR_* */
    int defvali;
    TupleValueType ctype;    /* Type of constant/def value */

    int fieldidx;                    /* if >= 0: Index # of "pre-defined" Tuple fields */
    bool fieldread, fieldvalid;
    char * fieldstr;
};

/* Initialize an evaluation context */
TupleEvalContext * tuple_evalctx_new (void)
{
    return g_array_new (false, true, sizeof (TupleEvalVar));
}

/* "Reset" the evaluation context */
void tuple_evalctx_reset (TupleEvalContext * ctx)
{
    for (unsigned i = 0; i < ctx->len; i ++)
    {
        TupleEvalVar * var = GET_VAR (ctx, i);

        var->fieldread = false;
        var->fieldvalid = false;
        str_unref (var->fieldstr);
        var->fieldstr = nullptr;
    }
}

/* Free an evaluation context and associated data */
void tuple_evalctx_free (TupleEvalContext * ctx)
{
    for (unsigned i = 0; i < ctx->len; i ++)
    {
        TupleEvalVar * var = GET_VAR (ctx, i);

        str_unref (var->name);
        str_unref (var->fieldstr);
    }

    g_array_free (ctx, true);
}

/* note: may invalidate TupleEvalVar pointers due to reallocation */
static int tuple_evalctx_add_var (TupleEvalContext * ctx, const char * name,
 TupleEvalType type, TupleValueType ctype)
{
    int field = -1;

    if (type == TUPLE_VAR_FIELD)
    {
        field = Tuple::field_by_name (name);

        if (field < 0)
            return -1;
    }

    int i = ctx->len;
    g_array_set_size (ctx, i + 1);

    TupleEvalVar * var = GET_VAR (ctx, i);

    var->name = str_get (name);
    var->type = type;
    var->fieldidx = field;
    var->ctype = ctype;

    switch (type)
    {
    case TUPLE_VAR_FIELD:
        var->ctype = Tuple::field_get_type (field);
        break;

    case TUPLE_VAR_CONST:
        if (ctype == TUPLE_INT)
            var->defvali = atoi (name);

        break;
    }

    return i;
}


static void tuple_evalnode_insert (TupleEvalNode * * nodes, TupleEvalNode * node)
{
    if (* nodes)
    {
        node->prev = (* nodes)->prev;
        (* nodes)->prev->next = node;
        (* nodes)->prev = node;
        node->next = nullptr;
    }
    else
    {
        * nodes = node;
        node->prev = node;
        node->next = nullptr;
    }
}

void tuple_evalnode_free (TupleEvalNode * expr)
{
    TupleEvalNode * curr = expr, * next;

    while (curr)
    {
        next = curr->next;
        str_unref (curr->text);

        if (curr->children)
            tuple_evalnode_free (curr->children);

        g_slice_free (TupleEvalNode, curr);
        curr = next;
    }
}

static bool tc_get_item (TupleEvalContext * ctx, const char * * str,
 char * buf, int max, char endch, bool * literal, const char * errstr,
 const char * item)
{
    int i = 0;
    const char * s = * str;
    char tmpendch;

    if (* s == '"')
    {
        if (* literal == false)
        {
            tuple_error (ctx, "Literal string value not allowed in '%s'.\n", item);
            return false;
        }

        s ++;
        * literal = true;
        tmpendch = '"';
    }
    else
    {
        * literal = false;
        tmpendch = endch;
    }

    if (* literal == false)
    {
        while (* s != '\0' && * s != tmpendch &&
         (g_ascii_isalnum (* s) || * s == '-') && i < (max - 1))
            buf[i ++] = * s ++;

        if (* s != tmpendch && * s != '}' && !g_ascii_isalnum (* s) && * s != '-')
        {
            tuple_error (ctx, "Invalid field '%s' in '%s'.\n", * str, item);
            return false;
        }

        if (*s != tmpendch)
        {
            tuple_error (ctx, "Expected '%c' in '%s'.\n", tmpendch, item);
            return false;
        }
    }
    else
    {
        while (* s != '\0' && * s != tmpendch && i < (max - 1))
        {
            if (* s == '\\')
                s++;

            buf[i ++] = * s ++;
        }
    }

    buf[i] = '\0';

    if (* literal)
    {
        if (* s != tmpendch)
        {
            tuple_error (ctx, "Expected literal string end ('%c') in '%s'.\n", tmpendch, item);
            return false;
        }

        s ++;
    }

    if (* s != endch)
    {
        tuple_error (ctx, "Expected '%c' after %s in '%s'\n", endch, errstr, item);
        return false;
    }

    * str = s;
    return true;
}


static int tc_get_variable (TupleEvalContext * ctx, char * name, TupleEvalType type)
{
    TupleValueType ctype = TUPLE_UNKNOWN;

    if (g_ascii_isdigit (name[0]))
    {
        ctype = TUPLE_INT;
        type = TUPLE_VAR_CONST;
    }
    else
        ctype = TUPLE_STRING;

    if (type != TUPLE_VAR_CONST)
    {
        for (unsigned i = 0; i < ctx->len; i ++)
        {
            TupleEvalVar * var = GET_VAR (ctx, i);

            if (var->type == type && ! strcmp (var->name, name))
                return i;
        }
    }

    return tuple_evalctx_add_var (ctx, name, type, ctype);
}

static TupleEvalNode * tuple_compiler_pass1 (int * level,
 TupleEvalContext * ctx, const char * * expression);

static bool tc_parse_construct (TupleEvalContext * ctx, TupleEvalNode * * res,
 const char * item, const char * * c, int * level, TupleEvalOp opcode)
{
    char tmps1[MAX_STR], tmps2[MAX_STR];
    bool literal1 = true, literal2 = true;

    if (tc_get_item (ctx, c, tmps1, MAX_STR, ',', & literal1, "tag1", item))
    {
        (* c) ++;

        if (tc_get_item (ctx, c, tmps2, MAX_STR, ':', & literal2, "tag2", item))
        {
            TupleEvalNode * tmp = g_slice_new0 (TupleEvalNode);
            (* c) ++;

            tmp->opcode = opcode;

            if ((tmp->var[0] = tc_get_variable (ctx, tmps1,
             literal1 ? TUPLE_VAR_CONST : TUPLE_VAR_FIELD)) < 0)
            {
                tuple_evalnode_free (tmp);
                tuple_error (ctx, "Invalid variable '%s' in '%s'.\n", tmps1, item);
                return false;
            }

            if ((tmp->var[1] = tc_get_variable (ctx, tmps2,
             literal2 ? TUPLE_VAR_CONST : TUPLE_VAR_FIELD)) < 0)
            {
                tuple_evalnode_free (tmp);
                tuple_error (ctx, "Invalid variable '%s' in '%s'.\n", tmps2, item);
                return false;
            }

            tmp->children = tuple_compiler_pass1 (level, ctx, c);
            tuple_evalnode_insert (res, tmp);
        }
        else
            return false;
    }
    else
        return false;

    return true;
}

/* Compile format expression into TupleEvalNode tree.
 * A "simple" straight compilation is sufficient in first pass, later
 * passes can perform subexpression removal and other optimizations. */
static TupleEvalNode * tuple_compiler_pass1 (int * level,
 TupleEvalContext * ctx, const char * * expression)
{
    TupleEvalNode * res = nullptr, * tmp = nullptr;
    const char * c = * expression, * item;
    char tmps1[MAX_STR];
    bool literal, end = false;

    (* level) ++;

    while (* c != '\0' && ! end)
    {
        tmp = nullptr;

        if (* c == '}')
        {
            c ++;
            (* level) --;
            end = true;
        }
        else if (* c == '$')
        {
            /* Expression? */
            item = c ++;

            if (* c == '{')
            {
                TupleEvalOp opcode;
                const char * expr = ++ c;

                switch (* c)
                {
                case '?':
                    c ++;
                    /* Exists? */
                    literal = false;

                    if (tc_get_item (ctx, & c, tmps1, MAX_STR, ':', & literal, "tag", item))
                    {
                        c ++;
                        tmp = g_slice_new0 (TupleEvalNode);
                        tmp->opcode = OP_EXISTS;

                        if ((tmp->var[0] = tc_get_variable (ctx, tmps1, TUPLE_VAR_FIELD)) < 0)
                        {
                            tuple_error (ctx, "Invalid variable '%s' in '%s'.\n", tmps1, expr);
                            goto ret_error;
                        }

                        tmp->children = tuple_compiler_pass1 (level, ctx, & c);
                        tuple_evalnode_insert (& res, tmp);
                    }
                    else
                        goto ret_error;

                    break;

                case '=':
                    c ++;

                    if (* c != '=')
                        goto ret_error;

                    c ++;

                    /* Equals? */
                    if (! tc_parse_construct (ctx, & res, item, & c, level, OP_EQUALS))
                        goto ret_error;

                    break;

                case '!':
                    c ++;

                    if (* c != '=')
                        goto ret_error;

                    c ++;

                    if (! tc_parse_construct (ctx, & res, item, & c, level, OP_NOT_EQUALS))
                        goto ret_error;

                    break;

                case '<':
                    c ++;

                    if (* c == '=')
                    {
                        opcode = OP_LTEQ;
                        c ++;
                    }
                    else
                        opcode = OP_LT;

                    if (! tc_parse_construct (ctx, & res, item, & c, level, opcode))
                        goto ret_error;

                    break;

                case '>':
                    c ++;

                    if (* c == '=')
                    {
                        opcode = OP_GTEQ;
                        c ++;
                    }
                    else
                        opcode = OP_GT;

                    if (! tc_parse_construct (ctx, & res, item, & c, level, opcode))
                        goto ret_error;

                    break;

                case '(':
                    c ++;

                    if (! strncmp (c, "empty)?", 7))
                    {
                        c += 7;
                        literal = false;

                        if (tc_get_item (ctx, & c, tmps1, MAX_STR, ':', & literal, "tag", item))
                        {
                            c ++;
                            tmp = g_slice_new0 (TupleEvalNode);
                            tmp->opcode = OP_IS_EMPTY;

                            if ((tmp->var[0] = tc_get_variable (ctx, tmps1, TUPLE_VAR_FIELD)) < 0)
                            {
                                tuple_error (ctx, "Invalid variable '%s' in '%s'.\n", tmps1, expr);
                                goto ret_error;
                            }

                            tmp->children = tuple_compiler_pass1 (level, ctx, & c);
                            tuple_evalnode_insert (& res, tmp);
                        }
                        else
                            goto ret_error;
                    }
                    else
                        goto ret_error;

                    break;

                default:
                    /* Get expression content */
                    literal = false;

                    if (tc_get_item (ctx, & c, tmps1, MAX_STR, '}', & literal, "field", item))
                    {
                        /* I HAS A FIELD - A field. You has it. */
                        tmp = g_slice_new0 (TupleEvalNode);
                        tmp->opcode = OP_FIELD;

                        if ((tmp->var[0] = tc_get_variable (ctx, tmps1, TUPLE_VAR_FIELD)) < 0)
                        {
                            tuple_error (ctx, "Invalid variable '%s' in '%s'.\n", tmps1, expr);
                            goto ret_error;
                        }

                        tuple_evalnode_insert (& res, tmp);
                        c ++;

                    }
                    else
                        goto ret_error;
                }
            }
            else
            {
                tuple_error (ctx, "Expected '{', got '%c' in '%s'.\n", * c, c);
                goto ret_error;
            }

        }
        else
        {
            /* Parse raw/literal text */
            int i = 0;

            while (* c != '\0' && * c != '$' && * c != '%' && * c != '}' && i < (MAX_STR - 1))
            {
                if (* c == '\\')
                    c ++;

                tmps1[i ++] = * c ++;
            }

            tmps1[i] = '\0';

            tmp = g_slice_new0 (TupleEvalNode);
            tmp->opcode = OP_RAW;
            tmp->text = str_get (tmps1);
            tuple_evalnode_insert (& res, tmp);
        }
    }

    if (* level <= 0)
    {
        tuple_error (ctx, "Syntax error! Uneven/unmatched nesting of elements in '%s'!\n", c);
        goto ret_error;
    }

    * expression = c;
    return res;

ret_error:
    tuple_evalnode_free (tmp);
    tuple_evalnode_free (res);
    return nullptr;
}

TupleEvalNode * tuple_formatter_compile (TupleEvalContext * ctx, const char * expr)
{
    int level = 0;
    const char * tmpexpr = expr;
    TupleEvalNode * res1;

    res1 = tuple_compiler_pass1 (& level, ctx, & tmpexpr);

    if (level != 1)
    {
        tuple_error (ctx, "Syntax error! Uneven/unmatched nesting of elements! (%d)\n", level);
        tuple_evalnode_free (res1);
        return nullptr;
    }

    return res1;
}

/* Fetch a tuple field value.  Return true if found. */
static bool tf_get_fieldval (TupleEvalVar * var, const Tuple & tuple)
{
    if (var->type != TUPLE_VAR_FIELD || var->fieldidx < 0)
        return false;

    if (var->fieldread)
        return var->fieldvalid;

    if (tuple.get_value_type (var->fieldidx) != var->ctype)
    {
        var->fieldread = true;
        var->fieldvalid = false;
        return false;
    }

    if (var->ctype == TUPLE_INT)
        var->defvali = tuple.get_int (var->fieldidx);
    else if (var->ctype == TUPLE_STRING)
        var->fieldstr = tuple.get_str (var->fieldidx).to_c ();

    var->fieldread = true;
    var->fieldvalid = true;
    return true;
}

/* Fetch string or int value of given variable, whatever type it might be.
 * Return VAR_* type for the variable. */
static TupleValueType tf_get_var (char * * tmps, int * tmpi,
 TupleEvalVar * var, const Tuple & tuple)
{
    TupleValueType type = TUPLE_UNKNOWN;
    * tmps = nullptr;
    * tmpi = 0;

    switch (var->type)
    {
    case TUPLE_VAR_CONST:
        switch (var->ctype)
        {
        case TUPLE_STRING:
            * tmps = var->name;
            break;

        case TUPLE_INT:
            * tmpi = var->defvali;
            break;

        default: /* Cannot happen */
            break;
        }

        type = var->ctype;
        break;

    case TUPLE_VAR_FIELD:
        if (tf_get_fieldval (var, tuple))
        {
            type = var->ctype;

            if (type == TUPLE_INT)
                * tmpi = var->defvali;
            else if (type == TUPLE_STRING)
                * tmps = var->fieldstr;
        }

        break;
    }

    return type;
}


/* Evaluate tuple in given TupleEval expression in given
 * context and return resulting string.
 */
static bool tuple_formatter_eval_do (TupleEvalContext * ctx,
 TupleEvalNode * expr, const Tuple & tuple, GString * out)
{
    TupleEvalNode * curr = expr;
    TupleEvalVar * var0, * var1;
    TupleValueType type0, type1;
    int tmpi0, tmpi1;
    char * tmps0, * tmps1, * tmps2;
    bool result;
    int resulti;

    if (! expr)
        return false;

    while (curr)
    {
        StringBuf tmps (0);
        const char * str = nullptr;

        switch (curr->opcode)
        {
        case OP_RAW:
            str = curr->text;
            break;

        case OP_FIELD:
            var0 = GET_VAR (ctx, curr->var[0]);

            if (tf_get_fieldval (var0, tuple))
            {
                switch (var0->ctype)
                {
                case TUPLE_STRING:
                    str = var0->fieldstr;
                    break;

                case TUPLE_INT:
                    tmps.steal (int_to_str (var0->defvali));
                    str = tmps;
                    break;

                default:
                    str = nullptr;
                }
            }

            break;

        case OP_EQUALS:
        case OP_NOT_EQUALS:
        case OP_LT:
        case OP_LTEQ:
        case OP_GT:
        case OP_GTEQ:
            var0 = GET_VAR (ctx, curr->var[0]);
            var1 = GET_VAR (ctx, curr->var[1]);

            type0 = tf_get_var (& tmps0, & tmpi0, var0, tuple);
            type1 = tf_get_var (& tmps1, & tmpi1, var1, tuple);
            result = false;

            if (type0 != TUPLE_UNKNOWN && type1 != TUPLE_UNKNOWN)
            {
                if (type0 == type1)
                {
                    if (type0 == TUPLE_STRING)
                        resulti = strcmp (tmps0, tmps1);
                    else
                        resulti = tmpi0 - tmpi1;
                }
                else
                {
                    if (type0 == TUPLE_INT)
                        resulti = tmpi0 - atoi (tmps1);
                    else
                        resulti = atoi (tmps0) - tmpi1;
                }

                switch (curr->opcode)
                {
                case OP_EQUALS:
                    result = (resulti == 0);
                    break;

                case OP_NOT_EQUALS:
                    result = (resulti != 0);
                    break;

                case OP_LT:
                    result = (resulti < 0);
                    break;

                case OP_LTEQ:
                    result = (resulti <= 0);
                    break;

                case OP_GT:
                    result = (resulti > 0);
                    break;

                case OP_GTEQ:
                    result = (resulti >= 0);
                    break;

                default:
                    result = false;
                }
            }

            if (result && ! tuple_formatter_eval_do (ctx, curr->children, tuple, out))
                return false;

            break;

        case OP_EXISTS:
            if (tf_get_fieldval (GET_VAR (ctx, curr->var[0]), tuple))
            {
                if (! tuple_formatter_eval_do (ctx, curr->children, tuple, out))
                    return false;
            }

            break;

        case OP_IS_EMPTY:
            var0 = GET_VAR (ctx, curr->var[0]);

            if (tf_get_fieldval (var0, tuple))
            {
                switch (var0->ctype)
                {
                case TUPLE_INT:
                    result = (var0->defvali == 0);
                    break;

                case TUPLE_STRING:
                    result = true;
                    tmps2 = var0->fieldstr;

                    while (result && tmps2 && *tmps2 != '\0')
                    {
                        if (g_ascii_isspace (* tmps2))
                            tmps2 ++;
                        else
                            result = false;
                    }

                    break;

                default:
                    result = true;
                }
            }
            else
                result = true;

            if (result && ! tuple_formatter_eval_do (ctx, curr->children, tuple, out))
                return false;

            break;

        default:
            /* should not be reached */
            return false;
        }

        if (str)
            g_string_append (out, str);

        curr = curr->next;
    }

    return true;
}

void tuple_formatter_eval (TupleEvalContext * ctx, TupleEvalNode * expr,
 const Tuple & tuple, GString * out)
{
    g_string_truncate (out, 0);
    tuple_formatter_eval_do (ctx, expr, tuple, out);
}

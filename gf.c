/*
  Generic Functions
  . method table and lookup
  . GF constructor, add_method
  . dispatch
  . static parameter inference
*/
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <assert.h>
#include <sys/types.h>
#include <limits.h>
#include <errno.h>
#include <math.h>
#include <gc.h>
#include "llt.h"
#include "julia.h"

static jl_methtable_t *new_method_table()
{
    jl_methtable_t *mt = (jl_methtable_t*)allocb(sizeof(jl_methtable_t));
    mt->mlist = NULL;
    mt->generics = NULL;
    return mt;
}

// takes arguments in the same order as jl_subtype()
typedef int (*jl_type_comparer_t)(jl_type_t *a, jl_type_t *b);

typedef int (*jl_argtuple_comparer_t)(jl_value_t **args, size_t n,
                                      jl_type_t *b);

// trivial linked list implementation for now
// TODO: pull out all the stops
jl_methlist_t *jl_method_list_assoc(jl_methlist_t *ml,
                                    jl_value_t **args, size_t n,
                                    jl_argtuple_comparer_t pred)
{
    while (ml != NULL) {
        if (pred(args, n, ml->sig))
            return ml;
        ml = ml->next;
    }
    return NULL;
}

static int args_match_sig(jl_value_t **a, size_t n, jl_type_t *b)
{
    assert(jl_is_tuple(b));
    return jl_tuple_subtype(a, n, &jl_tupleref(b,0), ((jl_tuple_t*)b)->length,
                            1, 0);
}

jl_function_t *jl_instantiate_method(jl_function_t *f, jl_value_pair_t *env)
{
    // TODO
    return f;
}

static int exact_arg_match(jl_value_t **args, size_t n, jl_tuple_t *sig)
{
    assert(jl_is_tuple(sig));
    if (sig->length != n) return 0;
    size_t i;
    for(i=0; i < n; i++) {
        // note: because this uses jl_typeof() directly, it never detects
        // exact matches for tuples. however this is a conservative answer
        // given the rest of the dispatch process.
        if (!jl_types_equal((jl_value_t*)jl_typeof(args[i]), jl_tupleref(sig,i)))
            return 0;
    }
    return 1;
}

jl_methlist_t *jl_method_table_insert(jl_methtable_t *mt, jl_type_t *type,
                                      jl_function_t *method);

jl_methlist_t *jl_method_table_assoc(jl_methtable_t *mt,
                                     jl_value_t **args, size_t nargs)
{
    /*
      search order:
      look at concrete signatures
      if there is an exact match, return it
      otherwise look for a matching generic signature
      if no concrete or generic match, raise error
      if there was no concrete match, use the generic one
      else use whichever of the concrete match or the instantiated generic one
        is more specific
      
      TODO: cache exact matches in a faster lookup table
    */
    jl_methlist_t *m = jl_method_list_assoc(mt->mlist, args, nargs,
                                            args_match_sig);
    if (m!=NULL && exact_arg_match(args, nargs, (jl_tuple_t*)m->sig))
        return m;
    
    // try generics
    jl_methlist_t *gm = mt->generics;
    // TODO: avoid this allocation
    jl_tuple_t *tt = jl_alloc_tuple(nargs);
    jl_value_pair_t *env = NULL;
    size_t i;
    for(i=0; i < tt->length; i++) {
        jl_tupleset(tt, i, (jl_value_t*)jl_full_type(args[i]));
    }
    while (gm != NULL) {
        env = jl_type_conform((jl_type_t*)tt, gm->sig);
        if (env != NULL) break;
        gm = gm->next;
    }
    if (env == NULL) {
        if (m != NULL) {
            // TODO: possibly re-specialize method on inexact match
        }
        return m;
    }
    // instantiate type signature and method with the parameter
    // assignments we found
    jl_value_pair_t *temp = env;
    size_t n=0;
    while (temp != NULL) {
        n++;
        temp = temp->next;
    }
    jl_value_t **tenv = (jl_value_t**)alloca(2 * n * sizeof(jl_value_t*));
    temp = env;
    for(i=0; i < n; i++) {
        tenv[i*2+0] = temp->a;
        tenv[i*2+1] = temp->b;
        temp = temp->next;
    }
    jl_type_t *newtype = jl_instantiate_type_with(gm->sig, tenv, n);

    assert(jl_subtype((jl_value_t*)tt, (jl_value_t*)newtype, 0, 0));

    if (m!=NULL && jl_subtype((jl_value_t*)m->sig, (jl_value_t*)newtype,0,0)) {
        // the inexact concrete method we found earlier is actually
        // more specific than this one
        return m;
    }

    // cache result in concrete part of method table
    jl_function_t *newmeth = jl_instantiate_method(gm->func, env);
    return jl_method_table_insert(mt, newtype, newmeth);
}

static int args_match_generic(jl_type_t *a, jl_type_t *b)
{
    return (jl_type_conform(a,b) != NULL);
}

static int args_match(jl_type_t *a, jl_type_t *b)
{
    return jl_subtype((jl_value_t*)a,(jl_value_t*)b,0,0);
}

static int sigs_match(jl_value_t **a, size_t n, jl_type_t *b)
{
    return jl_tuple_subtype(a, n, &jl_tupleref(b,0), ((jl_tuple_t*)b)->length,
                            0, 0) &&
        jl_tuple_subtype(&jl_tupleref(b,0), ((jl_tuple_t*)b)->length, a, n,
                         0, 0);
}

static
jl_methlist_t *jl_method_list_insert_p(jl_methlist_t **pml, jl_type_t *type,
                                       jl_function_t *method,
                                       jl_type_comparer_t pred)
{
    jl_methlist_t *l, **pl;

    l = jl_method_list_assoc(*pml,
                             &jl_tupleref(type,0), ((jl_tuple_t*)type)->length,
                             sigs_match);
    if (l != NULL) {
        // method overwritten
        l->sig = type;
        l->func = method;
        return l;
    }
    jl_methlist_t *newrec = (jl_methlist_t*)allocb(sizeof(jl_methlist_t));
    newrec->sig = type;
    newrec->func = method;
    pl = pml;
    l = *pml;
    while (l != NULL) {
        if (pred(type, l->sig))
            break;
        pl = &l->next;
        l = l->next;
    }
    newrec->next = l;
    *pl = newrec;
    return newrec;
}

jl_methlist_t *jl_method_table_insert(jl_methtable_t *mt, jl_type_t *type,
                                      jl_function_t *method)
{
    /*
      TODO: warn about ambiguous method priorities
      
      the relative priority of A and B is ambiguous if
      !subtype(A,B) && !subtype(B,A) && no corresponding tuple
      elements are disjoint.
      
      for example, (Tensor, Matrix) and (Matrix, Tensor) are ambiguous.
      however, (Tensor, Matrix, Foo) and (Matrix, Tensor, Bar) are fine
      since Foo and Bar are disjoint, so there would be no confusion over
      which one to call.
    */
    if (jl_has_typevars((jl_value_t*)type)) {
        return jl_method_list_insert_p(&mt->generics, type, method,
                                       args_match_generic);
    }
    else {
        return jl_method_list_insert_p(&mt->mlist, type, method, args_match);
    }
}

JL_CALLABLE(jl_apply_generic)
{
    jl_methtable_t *mt = (jl_methtable_t*)((jl_value_pair_t*)env)->a;

    jl_methlist_t *m = jl_method_table_assoc(mt, args, nargs);

    if (m == NULL) {
        jl_sym_t *name = (jl_sym_t*)((jl_value_pair_t*)env)->b;
        jl_errorf("No matching method for function %s", name->name);
    }

    return (m->func->fptr)(m->func->env, args, nargs);
}

jl_function_t *jl_new_generic_function(jl_sym_t *name)
{
    jl_value_pair_t *vp = (jl_value_pair_t*)allocb(sizeof(jl_value_pair_t));
    vp->a = (jl_value_t*)new_method_table();
    vp->b = (jl_value_t*)name;

    return jl_new_closure(jl_apply_generic, (jl_value_t*)vp);
}

static jl_value_t *dummy_tvar(jl_type_t *lb, jl_type_t *ub)
{
    return jl_new_struct(jl_tvar_type, jl_gensym(), lb, ub);
}

static jl_value_t *add_dummy_type_vars(jl_value_t *t)
{
    size_t i;
    if (jl_is_typector(t)) {
        jl_typector_t *tc = (jl_typector_t*)t;
        jl_tuple_t *p = jl_alloc_tuple(tc->parameters->length);
        for(i=0; i < p->length; i++) {
            jl_tvar_t *tv = (jl_tvar_t*)jl_tupleref(tc->parameters,i);
            jl_tupleset(p, i, dummy_tvar(tv->lb, tv->ub));
        }
        return (jl_value_t*)jl_apply_type_ctor(tc, p);
    }
    else if (jl_is_tuple(t)) {
        jl_tuple_t *p = (jl_tuple_t*)t;
        jl_tuple_t *np = jl_alloc_tuple(p->length);
        for(i=0; i < p->length; i++)
            jl_tupleset(np, i, add_dummy_type_vars(jl_tupleref(p,i)));
        return (jl_value_t*)np;
    }
    else if (jl_is_union_type(t)) {
        return jl_new_uniontype((jl_tuple_t*)add_dummy_type_vars(((jl_uniontype_t*)t)->types));
    }
    else if (jl_is_func_type(t)) {
        return jl_new_functype((jl_type_t*)add_dummy_type_vars(((jl_func_type_t*)t)->from),
                               (jl_type_t*)add_dummy_type_vars(((jl_func_type_t*)t)->to));
    }
    return t;
}

void jl_add_method(jl_function_t *gf, jl_tuple_t *types, jl_function_t *meth)
{
    assert(jl_is_gf(gf));
    assert(jl_is_tuple(types));
    assert(jl_is_func(meth));
    types = (jl_tuple_t*)add_dummy_type_vars((jl_value_t*)types);
    (void)jl_method_table_insert(jl_gf_mtable(gf), (jl_type_t*)types, meth);
}

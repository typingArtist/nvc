//
//  Copyright (C) 2011  Nick Gasson
//
//  This program is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include "tree.h"
#include "util.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CONTEXTS 16
#define MAX_ATTRS    16

//#define EXTRA_READ_CHECKS

struct tree_array {
   size_t count;
   size_t max;
   tree_t *items;
};

struct param_array {
   size_t  count;
   size_t  max;
   param_t *items;
};

typedef enum {
   A_STRING, A_INT, A_PTR, A_TREE
} attr_kind_t;

struct attr {
   attr_kind_t kind;
   ident_t     name;
   union {
      char   *sval;
      int    ival;
      void   *pval;
      tree_t tval;
   };
};

struct tree {
   tree_kind_t kind;
   loc_t       loc;
   ident_t     ident;
   struct attr *attrs;
   unsigned    n_attrs;

   union {
      struct tree_array  ports;    // T_ENTITY, T_FUNC_DECL, T_FBODY
      struct param_array params;   // T_FCALL, T_ATTR_REF
      struct tree_array  decls;    // T_ARCH, T_PROCESS, T_PACKAGE
      struct tree_array  drivers;  // T_SIGNAL_DECL
   };
   union {
      struct tree_array  generics; // T_ENTITY
      struct tree_array  stmts;    // T_ARCH, T_PROCESS, T_PACKAGE, T_FBODY
      struct tree_array  waves;    // T_SIGNAL_ASSIGN, T_CASSSING
      struct param_array genmaps;  // T_INSTANCE
   };
   union {
      literal_t   literal;         // T_LITERAL
      port_mode_t port_mode;       // T_PORT_MODE
      ident_t     ident2;          // T_ARCH, T_ATTR_REF
      tree_t      message;         // T_ASSERT
      tree_t delay;                // T_WAIT
   };
   union {
      tree_t   target;             // T_VAR_ASSIGN, T_SIGNAL_ASSIGN
      tree_t   ref;                // T_REF, T_FCALL, T_ARRAY_REF
      tree_t   severity;           // T_ASSERT
      unsigned pos;                // T_ENUM_LIT;
   };
   union {
      struct {                     // T_AGGREGATE
         assoc_t  *assocs;
         unsigned n_assocs;
         unsigned n_assocs_alloc;
      };
      struct {                     // T_ARCH, T_ENTITY, T_PACKAGE
         context_t *context;
         unsigned  n_contexts;
      };
      struct {                     // T_SIGNAL_DECL
         struct tree_array *sub_drivers;
         unsigned          n_elems;
      };
      range_t           range;     // T_ARRAY_SLICE
      struct tree_array triggers;  // T_WAIT, T_PROCESS
      struct tree_array elses;     // T_IF
   };
   type_t type;                    // many
   tree_t value;                   // many

   // Serialisation and GC bookkeeping
   unsigned short generation;
   uint32_t       index;
};

struct tree_wr_ctx {
   FILE          *file;
   type_wr_ctx_t type_ctx;
   unsigned      generation;
   unsigned      n_trees;
};

struct tree_rd_ctx {
   FILE          *file;
   type_rd_ctx_t type_ctx;
   unsigned      n_trees;
   tree_t        *store;
   unsigned      store_sz;
};

#define IS(t, k) ((t)->kind == (k))
#define IS_TOP_LEVEL(t)                                  \
   (IS(t, T_ARCH) || IS(t, T_ENTITY) || IS(t, T_PACKAGE) \
    || IS(t, T_ELAB) || IS(t, T_PBODY))
#define IS_DECL(t)                                                     \
   (IS(t, T_PORT_DECL) || IS(t, T_SIGNAL_DECL) || IS(t, T_VAR_DECL)    \
    || IS(t, T_TYPE_DECL) || IS(t, T_CONST_DECL) || IS(t, T_FUNC_DECL) \
    || IS(t, T_FBODY) || IS(t, T_ALIAS))
#define IS_EXPR(t)                                                    \
   (IS(t, T_FCALL) || IS(t, T_LITERAL) || IS(t, T_REF)                \
    || IS(t, T_QUALIFIED) || IS(t, T_AGGREGATE) || IS(t, T_ATTR_REF)  \
    || IS(t, T_ARRAY_REF) || IS(t, T_ARRAY_SLICE))
#define IS_STMT(t)                                                    \
   (IS(t, T_PROCESS) || IS(t, T_WAIT) || IS(t, T_VAR_ASSIGN)          \
    || IS(t, T_SIGNAL_ASSIGN) || IS(t, T_ASSERT) || IS(t, T_INSTANCE) \
    || IS(t, T_IF) || IS(t, T_NULL) || IS(t, T_RETURN)                \
    || IS(t, T_CASSIGN) || IS(t, T_WHILE) || IS(t, T_FOR))
#define HAS_IDENT(t)                                                  \
   (IS(t, T_ENTITY) || IS(t, T_PORT_DECL) || IS(t, T_FCALL)           \
    || IS(t, T_ARCH) || IS(t, T_SIGNAL_DECL) || IS_STMT(t)            \
    || IS(t, T_VAR_DECL) || IS(t, T_REF) || IS(t, T_TYPE_DECL)        \
    || IS(t, T_PACKAGE) || IS(t, T_QUALIFIED) || IS(t, T_ENUM_LIT)    \
    || IS(t, T_CONST_DECL) || IS(t, T_FUNC_DECL) || IS(t, T_ELAB)     \
    || IS(t, T_ATTR_REF) || IS(t, T_INSTANCE) || IS(t, T_PBODY)       \
    || IS(t, T_FBODY) || IS(t, T_CASSIGN) || IS(t, T_WHILE)           \
    || IS(t, T_ALIAS))
#define HAS_IDENT2(t)                                               \
   (IS(t, T_ARCH) || IS(t, T_ATTR_REF) || IS(t, T_INSTANCE)         \
    || IS(t, T_FOR))
#define HAS_PORTS(t) \
   (IS(t, T_ENTITY) || IS(t, T_FUNC_DECL) || IS(t, T_FBODY))
#define HAS_GENERICS(t) (IS(t, T_ENTITY))
#define HAS_TYPE(t)                                                 \
   (IS(t, T_PORT_DECL) || IS(t, T_SIGNAL_DECL) || IS(t, T_VAR_DECL) \
    || IS(t, T_TYPE_DECL) || IS_EXPR(t) || IS(t, T_ENUM_LIT)        \
    || IS(t, T_CONST_DECL) || IS(t, T_FUNC_DECL) || IS(t, T_FBODY)  \
    || IS(t, T_ALIAS))
#define HAS_PARAMS(t) \
   (IS(t, T_FCALL) || IS(t, T_ATTR_REF) || IS(t, T_ARRAY_REF) \
    || IS(t, T_INSTANCE))
#define HAS_DECLS(t) \
   (IS(t, T_ARCH) || IS(t, T_PROCESS) || IS(t, T_PACKAGE) || IS(t, T_ELAB) \
    || IS(t, T_PBODY) || IS(t, T_FOR))
#define HAS_TRIGGERS(t) (IS(t, T_WAIT) || IS(t, T_PROCESS))
#define HAS_STMTS(t)                                                  \
   (IS(t, T_ARCH) || IS(t, T_PROCESS) || IS(t, T_ELAB) || IS(t, T_IF) \
    || IS(t, T_FBODY) || IS(t, T_WHILE) || IS(t, T_FOR))
#define HAS_DELAY(t) (IS(t, T_WAIT) || IS(t, T_WAVEFORM))
#define HAS_TARGET(t) \
   (IS(t, T_VAR_ASSIGN) || IS(t, T_SIGNAL_ASSIGN) || IS(t, T_CASSIGN))
#define HAS_VALUE(t)                                                   \
   (IS_DECL(t) || IS(t, T_VAR_ASSIGN) || IS(t, T_WAVEFORM)             \
    || IS(t, T_QUALIFIED) || IS(t, T_CONST_DECL) || IS(t, T_ASSERT)    \
    || IS(t, T_ATTR_REF) || IS(t, T_ARRAY_REF) || IS(t, T_ARRAY_SLICE) \
    || IS(t, T_IF) || IS(t, T_RETURN) || IS(t, T_WHILE)                \
    || IS(t, T_ALIAS))
#define HAS_CONTEXT(t)                                          \
   (IS(t, T_ARCH) || IS(t, T_ENTITY) || IS(t, T_PACKAGE)        \
    || IS(t, T_PBODY))
#define HAS_REF(t) \
   (IS(t, T_REF) || IS(t, T_FCALL) || IS(t, T_ATTR_REF) || IS(t, T_ARRAY_REF) \
    || IS(t, T_ARRAY_SLICE) || IS(t, T_INSTANCE))
#define HAS_WAVEFORMS(t) (IS(t, T_SIGNAL_ASSIGN) || IS(t, T_CASSIGN))
#define HAS_RANGE(t) (IS(t, T_ARRAY_SLICE) || IS(t, T_FOR))

#define TREE_ARRAY_BASE_SZ  16

// Garbage collection
static tree_t   *all_trees = NULL;
static size_t   max_trees = 128;   // Grows at runtime
static size_t   n_trees_alloc = 0;
static unsigned next_generation = 1;

static unsigned tree_visit_aux(tree_t t, tree_visit_fn_t fn, void *context,
                               tree_kind_t kind, unsigned generation,
                               bool deep);

static void tree_array_add(struct tree_array *a, tree_t t)
{
   if (a->max == 0) {
      a->items = xmalloc(sizeof(tree_t) * TREE_ARRAY_BASE_SZ);
      a->max   = TREE_ARRAY_BASE_SZ;
   }
   else if (a->count == a->max) {
      a->max *= 2;
      a->items = xrealloc(a->items, sizeof(tree_t) * a->max);
   }

   a->items[a->count++] = t;
}

static inline tree_t tree_array_nth(struct tree_array *a, unsigned n)
{
   assert(n < a->count);
   return a->items[n];
}

static void param_array_add(struct param_array *a, param_t p)
{
   if (a->max == 0) {
      a->items = xmalloc(sizeof(param_t) * TREE_ARRAY_BASE_SZ);
      a->max   = TREE_ARRAY_BASE_SZ;
   }
   else if (a->count == a->max) {
      a->max *= 2;
      a->items = xrealloc(a->items, sizeof(param_t) * a->max);
   }

   a->items[a->count++] = p;
}

tree_t tree_new(tree_kind_t kind)
{
   assert(kind < T_LAST_TREE_KIND);

   tree_t t = xmalloc(sizeof(struct tree));
   memset(t, '\0', sizeof(struct tree));
   t->kind  = kind;
   t->index = UINT32_MAX;

   if (all_trees == NULL)
      all_trees = xmalloc(sizeof(tree_t) * max_trees);
   else if (n_trees_alloc == max_trees) {
      max_trees *= 2;
      all_trees = xrealloc(all_trees, sizeof(tree_t) * max_trees);
   }
   all_trees[n_trees_alloc++] = t;

   return t;
}

void tree_gc(void)
{
   // Generation will be updated by tree_visit
   const unsigned base_gen = next_generation;

   // Mark
   for (unsigned i = 0; i < n_trees_alloc; i++) {
      assert(all_trees[i] != NULL);

      if (IS_TOP_LEVEL(all_trees[i]))
         tree_visit_aux(all_trees[i], NULL, NULL,
                        (tree_kind_t)T_LAST_TREE_KIND,
                        next_generation++,
                        true);
   }

   // Sweep
   for (unsigned i = 0; i < n_trees_alloc; i++) {
      tree_t t = all_trees[i];
      if (t->generation < base_gen) {
         if (HAS_TYPE(t) && t->type != NULL)
            type_unref(t->type);

         if (HAS_PORTS(t) && t->ports.items != NULL)
            free(t->ports.items);
         if (HAS_GENERICS(t) && t->generics.items != NULL)
            free(t->generics.items);
         if (HAS_PARAMS(t) && t->params.items != NULL)
            free(t->params.items);
         if (HAS_DECLS(t) && t->decls.items != NULL)
            free(t->decls.items);
         if (HAS_STMTS(t) && t->stmts.items != NULL)
            free(t->stmts.items);

         if (HAS_CONTEXT(t) && t->context != NULL)
            free(t->context);

         if (t->attrs != NULL) {
            for (unsigned i = 0; i < t->n_attrs; i++) {
               if (t->attrs[i].kind == A_STRING)
                  free(t->attrs[i].sval);
            }
            free(t->attrs);
         }

         free(t);

         all_trees[i] = NULL;
      }
   }

   // Compact
   size_t p = 0;
   for (unsigned i = 0; i < n_trees_alloc; i++) {
      if (all_trees[i] != NULL)
         all_trees[p++] = all_trees[i];
   }

   printf("[gc: freed %zu trees; %zu allocated]\n",
          n_trees_alloc - p, p);

   n_trees_alloc = p;
}

const loc_t *tree_loc(tree_t t)
{
   assert(t != NULL);

   return &t->loc;
}

void tree_set_loc(tree_t t, const loc_t *loc)
{
   assert(t != NULL);
   assert(loc != NULL);

   t->loc = *loc;
}

ident_t tree_ident(tree_t t)
{
   assert(t != NULL);
   assert(HAS_IDENT(t));
   assert(t->ident != NULL);

   return t->ident;
}

bool tree_has_ident(tree_t t)
{
   assert(t != NULL);
   assert(HAS_IDENT(t));

   return t->ident != NULL;
}

void tree_set_ident(tree_t t, ident_t i)
{
   assert(t != NULL);
   assert(i != NULL);
   assert(HAS_IDENT(t));

   t->ident = i;
}

ident_t tree_ident2(tree_t t)
{
   assert(t != NULL);
   assert(HAS_IDENT2(t));
   assert(t->ident2 != NULL);

   return t->ident2;
}

void tree_set_ident2(tree_t t, ident_t i)
{
   assert(t != NULL);
   assert(i != NULL);
   assert(HAS_IDENT2(t));

   t->ident2 = i;
}

tree_kind_t tree_kind(tree_t t)
{
   assert(t != NULL);
   return t->kind;
}

void tree_change_kind(tree_t t, tree_kind_t kind)
{
   assert(t != NULL);
   t->kind = kind;
}

unsigned tree_ports(tree_t t)
{
   assert(t != NULL);
   assert(HAS_PORTS(t));

   return t->ports.count;
}

tree_t tree_port(tree_t t, unsigned n)
{
   assert(t != NULL);
   assert(HAS_PORTS(t));

   return tree_array_nth(&t->ports, n);
}

void tree_add_port(tree_t t, tree_t d)
{
   assert(t != NULL);
   assert(d != NULL);
   assert(HAS_PORTS(t));
   assert(IS_DECL(d));

   tree_array_add(&t->ports, d);
}

port_mode_t tree_port_mode(tree_t t)
{
   assert(t != NULL);
   assert(IS(t, T_PORT_DECL));
   assert(t->port_mode != PORT_INVALID);

   return t->port_mode;
}

void tree_set_port_mode(tree_t t, port_mode_t mode)
{
   assert(t != NULL);
   assert(IS(t, T_PORT_DECL));

   t->port_mode = mode;
}

unsigned tree_generics(tree_t t)
{
   assert(t != NULL);
   assert(HAS_GENERICS(t));

   return t->generics.count;
}

tree_t tree_generic(tree_t t, unsigned n)
{
   assert(t != NULL);
   assert(HAS_GENERICS(t));

   return tree_array_nth(&t->generics, n);
}

void tree_add_generic(tree_t t, tree_t d)
{
   assert(t != NULL);
   assert(d != NULL);
   assert(HAS_GENERICS(t));
   assert(IS_DECL(d));

   tree_array_add(&t->generics, d);
}

type_t tree_type(tree_t t)
{
   assert(t != NULL);
   assert(HAS_TYPE(t));
   assert(t->type != NULL);

   return t->type;
}

void tree_set_type(tree_t t, type_t ty)
{
   assert(t != NULL);
   assert(HAS_TYPE(t));

   type_ref(ty);
   if (t->type)
      type_unref(t->type);
   t->type = ty;
}

bool tree_has_type(tree_t t)
{
   assert(t != NULL);
   assert(HAS_TYPE(t));

   return t->type != NULL;
}

unsigned tree_params(tree_t t)
{
   assert(t != NULL);
   assert(HAS_PARAMS(t));

   return t->params.count;
}

param_t tree_param(tree_t t, unsigned n)
{
   assert(t != NULL);
   assert(HAS_PARAMS(t));
   assert(n < t->params.count);

   return t->params.items[n];
}

void tree_add_param(tree_t t, param_t e)
{
   assert(t != NULL);
   assert(HAS_PARAMS(t));
   assert(e.kind == P_RANGE || IS_EXPR(e.value));

   if (e.kind == P_POS)
      e.pos = t->params.count;

   param_array_add(&t->params, e);
}

unsigned tree_genmaps(tree_t t)
{
   assert(t != NULL);
   assert(IS(t, T_INSTANCE));

   return t->genmaps.count;
}

param_t tree_genmap(tree_t t, unsigned n)
{
   assert(t != NULL);
   assert(IS(t, T_INSTANCE));
   assert(n < t->genmaps.count);

   return t->genmaps.items[n];
}

void tree_add_genmap(tree_t t, param_t e)
{
   assert(t != NULL);
   assert(IS(t, T_INSTANCE));
   assert(e.kind == P_RANGE || IS_EXPR(e.value));

   if (e.kind == P_POS)
      e.pos = t->genmaps.count;

   param_array_add(&t->genmaps, e);
}

void tree_set_literal(tree_t t, literal_t lit)
{
   assert(t != NULL);
   assert(IS(t, T_LITERAL));

   t->literal = lit;
}

literal_t tree_literal(tree_t t)
{
   assert(t != NULL);
   assert(IS(t, T_LITERAL));

   return t->literal;
}

bool tree_has_value(tree_t t)
{
   assert(t != NULL);
   assert(HAS_VALUE(t));

   return t->value != NULL;
}

tree_t tree_value(tree_t t)
{
   assert(t != NULL);
   assert(HAS_VALUE(t));
   assert(t->value != NULL);

   return t->value;
}

void tree_set_value(tree_t t, tree_t v)
{
   assert(t != NULL);
   assert(HAS_VALUE(t));
   assert(v == NULL || IS_EXPR(v));

   t->value = v;
}

unsigned tree_decls(tree_t t)
{
   assert(t != NULL);
   assert(HAS_DECLS(t));

   return t->decls.count;
}

tree_t tree_decl(tree_t t, unsigned n)
{
   assert(t != NULL);
   assert(HAS_DECLS(t));

   return tree_array_nth(&t->decls, n);
}

void tree_add_decl(tree_t t, tree_t d)
{
   assert(t != NULL);
   assert(d != NULL);
   assert(HAS_DECLS(t));
   assert(IS_DECL(d));

   tree_array_add(&t->decls, d);
}

unsigned tree_stmts(tree_t t)
{
   assert(t != NULL);
   assert(HAS_STMTS(t));

   return t->stmts.count;
}

tree_t tree_stmt(tree_t t, unsigned n)
{
   assert(t != NULL);
   assert(HAS_STMTS(t));

   return tree_array_nth(&t->stmts, n);
}

void tree_add_stmt(tree_t t, tree_t s)
{
   assert(t != NULL);
   assert(s != NULL);
   assert(HAS_STMTS(t));
   assert(IS_STMT(s));

   tree_array_add(&t->stmts, s);
}

unsigned tree_waveforms(tree_t t)
{
   assert(t != NULL);
   assert(HAS_WAVEFORMS(t));

   return t->waves.count;
}

tree_t tree_waveform(tree_t t, unsigned n)
{
   assert(t != NULL);
   assert(HAS_WAVEFORMS(t));

   return tree_array_nth(&t->waves, n);
}

void tree_add_waveform(tree_t t, tree_t w)
{
   assert(t != NULL);
   assert(w != NULL);
   assert(HAS_WAVEFORMS(t));
   assert(IS(w, T_WAVEFORM));

   tree_array_add(&t->waves, w);
}

unsigned tree_else_stmts(tree_t t)
{
   assert(t != NULL);
   assert(IS(t, T_IF));

   return t->elses.count;
}

tree_t tree_else_stmt(tree_t t, unsigned n)
{
   assert(t != NULL);
   assert(IS(t, T_IF));

   return tree_array_nth(&t->elses, n);
}

void tree_add_else_stmt(tree_t t, tree_t s)
{
   assert(t != NULL);
   assert(s != NULL);
   assert(IS(t, T_IF));
   assert(IS_STMT(s));

   tree_array_add(&t->elses, s);
}

unsigned tree_drivers(tree_t t)
{
   assert(t != NULL);
   assert(IS(t, T_SIGNAL_DECL));

   return t->drivers.count;
}

tree_t tree_driver(tree_t t, unsigned n)
{
   assert(t != NULL);
   assert(IS(t, T_SIGNAL_DECL));

   return tree_array_nth(&t->drivers, n);
}

void tree_add_driver(tree_t t, tree_t d)
{
   assert(t != NULL);
   assert(d != NULL);
   assert(IS(t, T_SIGNAL_DECL));
   assert(IS(d, T_PROCESS));

   tree_array_add(&t->drivers, d);
}

unsigned tree_sub_drivers(tree_t t, unsigned elem)
{
   assert(t != NULL);
   assert(IS(t, T_SIGNAL_DECL));

   if (t->sub_drivers == NULL || elem >= t->n_elems)
      return 0;
   else
      return t->sub_drivers[elem].count;
}

tree_t tree_sub_driver(tree_t t, unsigned elem, unsigned n)
{
   assert(t != NULL);
   assert(IS(t, T_SIGNAL_DECL));
   assert(elem < t->n_elems);

   return tree_array_nth(&t->sub_drivers[elem], n);
}

void tree_add_sub_driver(tree_t t, unsigned elem, tree_t p)
{
   assert(t != NULL);
   assert(IS(t, T_SIGNAL_DECL));
   assert(IS(p, T_PROCESS));

   if (elem >= t->n_elems) {
      // TODO: growing by 1 each time is pretty inefficient
      //  -> add tree_sub_driver_hint(tree_t, unsigned)
      t->sub_drivers = xrealloc(t->sub_drivers,
                                (elem + 1) * sizeof(struct tree_array));
      memset(&t->sub_drivers[t->n_elems], '\0',
             (elem + 1 - t->n_elems) * sizeof(struct tree_array));
      t->n_elems = elem + 1;
   }

   tree_array_add(&t->sub_drivers[elem], p);
}

bool tree_has_delay(tree_t t)
{
   assert(t != NULL);
   assert(HAS_DELAY(t));

   return t->delay != NULL;
}

tree_t tree_delay(tree_t t)
{
   assert(t != NULL);
   assert(HAS_DELAY(t));
   assert(t->delay != NULL);

   return t->delay;
}

void tree_set_delay(tree_t t, tree_t d)
{
   assert(t != NULL);
   assert(d != NULL);
   assert(HAS_DELAY(t));
   assert(IS_EXPR(d));

   t->delay = d;
}

unsigned tree_triggers(tree_t t)
{
   assert(t != NULL);
   assert(HAS_TRIGGERS(t));

   return t->triggers.count;
}

tree_t tree_trigger(tree_t t, unsigned n)
{
   assert(t != NULL);
   assert(HAS_TRIGGERS(t));

   return tree_array_nth(&t->triggers, n);
}

void tree_add_trigger(tree_t t, tree_t s)
{
   assert(t != NULL);
   assert(s != NULL);
   assert(HAS_TRIGGERS(t));
   assert(IS_EXPR(s));

   tree_array_add(&t->triggers, s);
}

tree_t tree_target(tree_t t)
{
   assert(t != NULL);
   assert(HAS_TARGET(t));
   assert(t->target != NULL);

   return t->target;
}

void tree_set_target(tree_t t, tree_t lhs)
{
   assert(t != NULL);
   assert(HAS_TARGET(t));

   t->target = lhs;
}

tree_t tree_ref(tree_t t)
{
   assert(t != NULL);
   assert(HAS_REF(t));
   assert(t->ref != NULL);

   return t->ref;
}

void tree_set_ref(tree_t t, tree_t decl)
{
   assert(t != NULL);
   assert(HAS_REF(t));
   assert(IS_DECL(decl) || IS(decl, T_ENUM_LIT) || IS_TOP_LEVEL(decl));

   t->ref = decl;
}

unsigned tree_contexts(tree_t t)
{
   assert(t != NULL);
   assert(HAS_CONTEXT(t));

   return t->n_contexts;
}

context_t tree_context(tree_t t, unsigned n)
{
   assert(t != NULL);
   assert(HAS_CONTEXT(t));
   assert(n < t->n_contexts);

   return t->context[n];
}

void tree_add_context(tree_t t, context_t ctx)
{
   assert(t != NULL);
   assert(HAS_CONTEXT(t));
   assert(t->n_contexts < MAX_CONTEXTS);

   if (t->n_contexts == 0)
      t->context = xmalloc(sizeof(ident_t) * MAX_CONTEXTS);

   t->context[t->n_contexts++] = ctx;
}

unsigned tree_assocs(tree_t t)
{
   assert(t != NULL);
   assert(IS(t, T_AGGREGATE));

   return t->n_assocs;
}

assoc_t tree_assoc(tree_t t, unsigned n)
{
   assert(t != NULL);
   assert(IS(t, T_AGGREGATE));
   assert(n < t->n_assocs);

   return t->assocs[n];
}

void tree_add_assoc(tree_t t, assoc_t a)
{
   assert(t != NULL);
   assert(IS(t, T_AGGREGATE));

   if (t->assocs == NULL) {
      t->n_assocs_alloc = 16;
      t->assocs = xmalloc(sizeof(assoc_t) * t->n_assocs_alloc);
   }
   else if (t->n_assocs == t->n_assocs_alloc) {
      t->n_assocs_alloc *= 2;
      t->assocs = xrealloc(t->assocs, sizeof(assoc_t) * t->n_assocs_alloc);
   }

   if (a.kind == A_POS) {
      unsigned pos = 0;
      for (unsigned i = 0; i < t->n_assocs; i++) {
         if (t->assocs[i].kind == A_POS)
            pos++;
      }

      a.pos = pos;
   }

   t->assocs[t->n_assocs++] = a;
}

tree_t tree_severity(tree_t t)
{
   assert(t != NULL);
   assert(IS(t, T_ASSERT));
   assert(t->severity != NULL);

   return t->severity;
}

void tree_set_severity(tree_t t, tree_t s)
{
   assert(t != NULL);
   assert(IS(t, T_ASSERT));
   assert(IS_EXPR(s));

   t->severity = s;
}

tree_t tree_message(tree_t t)
{
   assert(t != NULL);
   assert(IS(t, T_ASSERT));
   assert(t->message != NULL);

   return t->message;
}

void tree_set_message(tree_t t, tree_t m)
{
   assert(t != NULL);
   assert(IS(t, T_ASSERT));
   assert(IS_EXPR(m));

   t->message = m;
}

range_t tree_range(tree_t t)
{
   assert(t != NULL);
   assert(HAS_RANGE(t));

   return t->range;
}

void tree_set_range(tree_t t, range_t r)
{
   assert(t != NULL);
   assert(HAS_RANGE(t));

   t->range = r;
}

unsigned tree_pos(tree_t t)
{
   assert(t != NULL);
   assert(IS(t, T_ENUM_LIT));

   return t->pos;
}

void tree_set_pos(tree_t t, unsigned pos)
{
   assert(t != NULL);
   assert(IS(t, T_ENUM_LIT));

   t->pos = pos;
}

uint32_t tree_index(tree_t t)
{
   assert(t != NULL);
   assert(t->index != UINT32_MAX);

   return t->index;
}

static unsigned tree_visit_a(struct tree_array *a,
                             tree_visit_fn_t fn, void *context,
                             tree_kind_t kind, unsigned generation,
                             bool deep)
{
   unsigned n = 0;
   for (unsigned i = 0; i < a->count; i++)
      n += tree_visit_aux(a->items[i], fn, context, kind,
                          generation, deep);

   return n;
}

static unsigned tree_visit_p(struct param_array *a,
                             tree_visit_fn_t fn, void *context,
                             tree_kind_t kind, unsigned generation,
                             bool deep)
{
   unsigned n = 0;
   for (unsigned i = 0; i < a->count; i++) {
      switch (a->items[i].kind) {
      case P_RANGE:
         n += tree_visit_aux(a->items[i].range.left,
                             fn, context, kind, generation, deep);
         n += tree_visit_aux(a->items[i].range.right,
                             fn, context, kind, generation, deep);
         break;

      case P_POS:
      case P_NAMED:
         n += tree_visit_aux(a->items[i].value,
                             fn, context, kind, generation, deep);
         break;
      }
   }

   return n;
}

static unsigned tree_visit_type(type_t type,
                                tree_visit_fn_t fn, void *context,
                                tree_kind_t kind, unsigned generation,
                                bool deep)
{
   if (type == NULL)
      return 0;

   unsigned n = 0;

   switch (type_kind(type)) {
   case T_SUBTYPE:
   case T_INTEGER:
   case T_PHYSICAL:
   case T_CARRAY:
      for (unsigned i = 0; i < type_dims(type); i++) {
         range_t r = type_dim(type, i);
         n += tree_visit_aux(r.left, fn, context, kind,
                             generation, deep);
         n += tree_visit_aux(r.right, fn, context, kind,
                             generation, deep);
      }
      break;

   default:
      break;
   }

   switch (type_kind(type)) {
   case T_SUBTYPE:
   case T_CARRAY:
   case T_UARRAY:
      n += tree_visit_type(type_base(type), fn, context, kind,
                           generation, deep);
      break;
   default:
      break;
   }

   switch (type_kind(type)) {
   case T_UNRESOLVED:
      break;

   case T_SUBTYPE:
      if (type_has_resolution(type))
         n += tree_visit_aux(type_resolution(type), fn, context,
                             kind, generation, deep);
      break;

   case T_PHYSICAL:
      for (unsigned i = 0; i < type_units(type); i++)
         n += tree_visit_aux(type_unit(type, i).multiplier, fn, context,
                             kind, generation, deep);
      break;

   case T_FUNC:
      for (unsigned i = 0; i < type_params(type); i++)
         n += tree_visit_type(type_param(type, i), fn, context,
                              kind, generation, deep);
      n += tree_visit_type(type_result(type), fn, context,
                           kind, generation, deep);
      break;

   case T_ENUM:
      for (unsigned i = 0; i < type_enum_literals(type); i++)
         n += tree_visit_aux(type_enum_literal(type, i), fn, context,
                             kind, generation, deep);
      break;

   case T_UARRAY:
      for (unsigned i = 0; i < type_index_constrs(type); i++)
         n += tree_visit_type(type_index_constr(type, i),
                              fn, context, kind, generation, deep);
      break;

   default:
      break;
   }

   return n;
}

static unsigned tree_visit_aux(tree_t t, tree_visit_fn_t fn, void *context,
                               tree_kind_t kind, unsigned generation,
                               bool deep)
{
   // If `deep' then will follow links above the tree originally passed
   // to tree_visit - e.g. following references back to their declarations
   // Outside the garbage collector this is usually not what is required

   if (t == NULL || t->generation == generation)
      return 0;

   t->generation = generation;

   unsigned n = 0;

   if (HAS_PORTS(t))
      n += tree_visit_a(&t->ports, fn, context, kind, generation, deep);
   if (HAS_GENERICS(t))
      n += tree_visit_a(&t->generics, fn, context, kind, generation, deep);
   if (HAS_DECLS(t))
      n += tree_visit_a(&t->decls, fn, context, kind, generation, deep);
   if (HAS_TRIGGERS(t))
      n += tree_visit_a(&t->triggers, fn, context, kind, generation, deep);
   if (HAS_STMTS(t))
      n += tree_visit_a(&t->stmts, fn, context, kind, generation, deep);
   if (HAS_WAVEFORMS(t))
      n += tree_visit_a(&t->waves, fn, context, kind, generation, deep);
   if (HAS_VALUE(t))
      n += tree_visit_aux(t->value, fn, context, kind, generation, deep);
   if (HAS_DELAY(t))
      n += tree_visit_aux(t->delay, fn, context, kind, generation, deep);
   if (HAS_TARGET(t))
      n += tree_visit_aux(t->target, fn, context, kind, generation, deep);
   if (HAS_REF(t) && deep)
      n += tree_visit_aux(t->ref, fn, context, kind, generation, deep);
   if (HAS_TYPE(t) && deep)
      n += tree_visit_type(t->type, fn, context, kind, generation, deep);
   if (HAS_PARAMS(t))
      n += tree_visit_p(&t->params, fn, context, kind, generation, deep);
   if (HAS_RANGE(t)) {
      n += tree_visit_aux(t->range.left, fn, context, kind,
                          generation, deep);
      n += tree_visit_aux(t->range.right, fn, context, kind,
                          generation, deep);
   }

   if (IS(t, T_ASSERT)) {
      n += tree_visit_aux(t->severity, fn, context,
                          kind, generation, deep);
      n += tree_visit_aux(t->message, fn, context,
                          kind, generation, deep);
   }
   else if (IS(t, T_AGGREGATE)) {
      for (unsigned n = 0; n < t->n_assocs; n++) {
         switch (t->assocs[n].kind) {
         case A_NAMED:
            tree_visit_aux(t->assocs[n].name, fn, context,
                           kind, generation, deep);
            break;
         case A_RANGE:
            tree_visit_aux(t->assocs[n].range.left, fn, context,
                           kind, generation, deep);
            tree_visit_aux(t->assocs[n].range.right, fn, context,
                           kind, generation, deep);
            break;
         default:
            break;
         }

         tree_visit_aux(t->assocs[n].value, fn, context,
                        kind, generation, deep);
      }
   }
   else if (IS(t, T_SIGNAL_DECL) && deep) {
      n += tree_visit_a(&t->drivers, fn, context, kind, generation, deep);
      for (unsigned i = 0; i < t->n_elems; i++)
         n += tree_visit_a(&t->sub_drivers[i], fn, context,
                           kind, generation, deep);
   }
   else if (IS(t, T_INSTANCE))
      n += tree_visit_p(&t->genmaps, fn, context, kind, generation, deep);
   else if (IS(t, T_IF))
      n += tree_visit_a(&t->elses, fn, context, kind, generation, deep);

   if (deep) {
      for (unsigned i = 0; i < t->n_attrs; i++) {
         switch (t->attrs[i].kind) {
         case A_TREE:
            tree_visit_aux(t->attrs[i].tval, fn, context,
                           kind, generation, deep);
            break;

         default:
            break;
         }
      }
   }

   if (t->kind == kind || kind == T_LAST_TREE_KIND) {
      if (fn)
         (*fn)(t, context);
      ++n;
   }

   return n;
}

unsigned tree_visit(tree_t t, tree_visit_fn_t fn, void *context)
{
   assert(t != NULL);

   return tree_visit_aux(t, fn, context,
                         (tree_kind_t)T_LAST_TREE_KIND,
                         next_generation++,
                         false);
}

unsigned tree_visit_only(tree_t t, tree_visit_fn_t fn,
                         void *context, tree_kind_t kind)
{
   assert(t != NULL);

   return tree_visit_aux(t, fn, context, kind,
                         next_generation++, false);
}

static void write_loc(loc_t *l, tree_wr_ctx_t ctx)
{
   write_s(l->first_line, ctx->file);
   write_s(l->first_column, ctx->file);
   write_s(l->last_line, ctx->file);
   write_s(l->last_column, ctx->file);
}

static loc_t read_loc(tree_rd_ctx_t ctx)
{
   loc_t l = { .file = "none", .linebuf = NULL };
   l.first_line   = read_s(ctx->file);
   l.first_column = read_s(ctx->file);
   l.last_line    = read_s(ctx->file);
   l.last_column  = read_s(ctx->file);
   return l;
}

static void write_a(struct tree_array *a, tree_wr_ctx_t ctx)
{
   write_u(a->count, ctx->file);
   for (unsigned i = 0; i < a->count; i++)
      tree_write(a->items[i], ctx);
}

static void read_a(struct tree_array *a, tree_rd_ctx_t ctx)
{
   a->count = a->max = read_u(ctx->file);
   a->items = xmalloc(a->count * sizeof(tree_t));
   for (unsigned i = 0; i < a->count; i++)
      a->items[i] = tree_read(ctx);
}

static void write_p(struct param_array *a, tree_wr_ctx_t ctx)
{
   write_u(a->count, ctx->file);
   for (unsigned i = 0; i < a->count; i++) {
      write_s(a->items[i].kind, ctx->file);
      switch (a->items[i].kind) {
      case P_POS:
         write_s(a->items[i].pos, ctx->file);
         tree_write(a->items[i].value, ctx);
         break;
      case P_RANGE:
         write_s(a->items[i].range.kind, ctx->file);
         tree_write(a->items[i].range.left, ctx);
         tree_write(a->items[i].range.right, ctx);
         break;
      case P_NAMED:
         ident_write(a->items[i].name, ctx->file);
         tree_write(a->items[i].value, ctx);
         break;
      }
   }
}

static void read_p(struct param_array *a, tree_rd_ctx_t ctx)
{
   a->max = a->count = read_u(ctx->file);
   a->items = xmalloc(sizeof(param_t) * a->count);

   for (unsigned i = 0; i < a->count; i++) {
      switch ((a->items[i].kind = read_s(ctx->file))) {
      case P_POS:
         a->items[i].pos   = read_s(ctx->file);
         a->items[i].value = tree_read(ctx);
         break;
      case P_RANGE:
         a->items[i].range.kind  = read_s(ctx->file);
         a->items[i].range.left  = tree_read(ctx);
         a->items[i].range.right = tree_read(ctx);
         break;
      case P_NAMED:
         a->items[i].name  = ident_read(ctx->file);
         a->items[i].value = tree_read(ctx);
         break;
      }
   }
}

tree_wr_ctx_t tree_write_begin(FILE *f)
{
   struct tree_wr_ctx *ctx = xmalloc(sizeof(struct tree_wr_ctx));
   ctx->file       = f;
   ctx->generation = next_generation++;
   ctx->n_trees    = 0;
   ctx->type_ctx   = type_write_begin(ctx);

   return ctx;
}

void tree_write_end(tree_wr_ctx_t ctx)
{
   type_write_end(ctx->type_ctx);
   free(ctx);
}

FILE *tree_write_file(tree_wr_ctx_t ctx)
{
   return ctx->file;
}

void tree_write(tree_t t, tree_wr_ctx_t ctx)
{
   if (t == NULL) {
      write_s(0xffff, ctx->file);  // Null marker
      return;
   }

   if (t->generation == ctx->generation) {
      // Already visited this tree
      write_s(0xfffe, ctx->file);   // Back reference marker
      write_u(t->index, ctx->file);
      return;
   }

   t->generation = ctx->generation;
   t->index      = (ctx->n_trees)++;

   write_s(t->kind, ctx->file);
   write_loc(&t->loc, ctx);
   if (HAS_IDENT(t))
      ident_write(t->ident, ctx->file);
   if (HAS_IDENT2(t))
      ident_write(t->ident2, ctx->file);
   if (HAS_PORTS(t))
      write_a(&t->ports, ctx);
   if (HAS_GENERICS(t))
      write_a(&t->generics, ctx);
   if (HAS_DECLS(t))
      write_a(&t->decls, ctx);
   if (HAS_TRIGGERS(t))
      write_a(&t->triggers, ctx);
   if (HAS_STMTS(t))
      write_a(&t->stmts, ctx);
   if (HAS_WAVEFORMS(t))
      write_a(&t->waves, ctx);
   if (HAS_TYPE(t))
      type_write(t->type, ctx->type_ctx);
   if (HAS_VALUE(t))
      tree_write(t->value, ctx);
   if (HAS_DELAY(t))
      tree_write(t->delay, ctx);
   if (HAS_TARGET(t))
      tree_write(t->target, ctx);
   if (HAS_REF(t))
      tree_write(t->ref, ctx);
   if (HAS_CONTEXT(t)) {
      write_s(t->n_contexts, ctx->file);
      for (unsigned i = 0; i < t->n_contexts; i++) {
         ident_write(t->context[i].name, ctx->file);
         write_loc(&t->context[i].loc, ctx);
      }
   }
   if (HAS_PARAMS(t))
      write_p(&t->params, ctx);
   if (HAS_RANGE(t)) {
      write_s(t->range.kind, ctx->file);
      tree_write(t->range.left, ctx);
      tree_write(t->range.right, ctx);
   }

   switch (t->kind) {
   case T_PORT_DECL:
      write_s(t->port_mode, ctx->file);
      break;

   case T_LITERAL:
      {
         write_s(t->literal.kind, ctx->file);
         switch (t->literal.kind) {
         case L_INT:
            write_i64(t->literal.i, ctx->file);
            break;
         default:
            abort();
         }
      }
      break;

   case T_AGGREGATE:
      {
         write_s(t->n_assocs, ctx->file);

         for (unsigned i = 0; i < t->n_assocs; i++) {
            write_s(t->assocs[i].kind, ctx->file);
            tree_write(t->assocs[i].value, ctx);

            switch (t->assocs[i].kind) {
            case A_POS:
               write_s(t->assocs[i].pos, ctx->file);
               break;
            case A_NAMED:
               tree_write(t->assocs[i].name, ctx);
               break;
            case A_RANGE:
               write_s(t->assocs[i].range.kind, ctx->file);
               tree_write(t->assocs[i].range.left, ctx);
               tree_write(t->assocs[i].range.right, ctx);
               break;
            case A_OTHERS:
               break;
            default:
               abort();
            }
         }
      }
      break;

   case T_ASSERT:
      tree_write(t->severity, ctx);
      tree_write(t->message, ctx);
      break;

   case T_ENUM_LIT:
      write_u(t->pos, ctx->file);
      break;

   case T_INSTANCE:
      write_p(&t->genmaps, ctx);
      break;

   case T_IF:
      write_a(&t->elses, ctx);
      break;

   default:
      break;
   }

   write_s(t->n_attrs, ctx->file);
   for (unsigned i = 0; i < t->n_attrs; i++) {
      write_s(t->attrs[i].kind, ctx->file);
      ident_write(t->attrs[i].name, ctx->file);

      switch (t->attrs[i].kind) {
      case A_STRING:
         write_s(strlen(t->attrs[i].sval), ctx->file);
         fputs(t->attrs[i].sval, ctx->file);
         break;

      case A_INT:
         write_i(t->attrs[i].ival, ctx->file);
         break;

      case A_TREE:
         tree_write(t->attrs[i].tval, ctx);
         break;

      case A_PTR:
         fatal("pointer attributes cannot be saved");
      }
   }

#ifdef EXTRA_READ_CHECKS
   write_s(0xdead, ctx->file);
#endif  // EXTRA_READ_CHECKS
}

tree_t tree_read(tree_rd_ctx_t ctx)
{
   unsigned short marker = read_s(ctx->file);
   if (marker == 0xffff)
      return NULL;    // Null marker
   else if (marker == 0xfffe) {
      // Back reference marker
      unsigned index = read_u(ctx->file);
      assert(index < ctx->n_trees);
      return ctx->store[index];
   }

   assert(marker < T_LAST_TREE_KIND);

   tree_t t = tree_new((tree_kind_t)marker);
   t->loc = read_loc(ctx);

   // Stash pointer for later back references
   // This must be done early as a child node of this type may
   // reference upwards
   t->index = ctx->n_trees++;
   if (ctx->n_trees == ctx->store_sz) {
      ctx->store_sz *= 2;
      ctx->store = xrealloc(ctx->store, ctx->store_sz * sizeof(tree_t));
   }
   ctx->store[t->index] = t;

   if (HAS_IDENT(t))
      tree_set_ident(t, ident_read(ctx->file));
   if (HAS_IDENT2(t))
      tree_set_ident2(t, ident_read(ctx->file));
   if (HAS_PORTS(t))
      read_a(&t->ports, ctx);
   if (HAS_GENERICS(t))
      read_a(&t->generics, ctx);
   if (HAS_DECLS(t))
      read_a(&t->decls, ctx);
   if (HAS_TRIGGERS(t))
      read_a(&t->triggers, ctx);
   if (HAS_STMTS(t))
      read_a(&t->stmts, ctx);
   if (HAS_WAVEFORMS(t))
      read_a(&t->waves, ctx);
   if (HAS_TYPE(t)) {
      if ((t->type = type_read(ctx->type_ctx)))
         type_ref(t->type);
   }
   if (HAS_VALUE(t))
      t->value = tree_read(ctx);
   if (HAS_DELAY(t))
      t->delay = tree_read(ctx);
   if (HAS_TARGET(t))
      t->target = tree_read(ctx);
   if (HAS_REF(t))
      t->ref = tree_read(ctx);
   if (HAS_CONTEXT(t)) {
      t->n_contexts = read_s(ctx->file);
      t->context    = xmalloc(sizeof(ident_t) * MAX_CONTEXTS);

      for (unsigned i = 0; i < t->n_contexts; i++) {
         t->context[i].name = ident_read(ctx->file);
         t->context[i].loc  = read_loc(ctx);
      }
   }
   if (HAS_PARAMS(t))
      read_p(&t->params, ctx);
   if (HAS_RANGE(t)) {
      t->range.kind  = read_s(ctx->file);
      t->range.left  = tree_read(ctx);
      t->range.right = tree_read(ctx);
   }

   switch (t->kind) {
   case T_PORT_DECL:
      t->port_mode = read_s(ctx->file);
      break;

   case T_LITERAL:
      {
         t->literal.kind = read_s(ctx->file);
         switch (t->literal.kind) {
         case L_INT:
            t->literal.i = read_i64(ctx->file);
            break;
         default:
            abort();
         }
      }
      break;

   case T_AGGREGATE:
      {
         t->n_assocs_alloc = t->n_assocs = read_s(ctx->file);
         t->assocs = xmalloc(sizeof(assoc_t) * t->n_assocs);

         for (unsigned i = 0; i < t->n_assocs; i++) {
            t->assocs[i].kind  = read_s(ctx->file);
            t->assocs[i].value = tree_read(ctx);

            switch (t->assocs[i].kind) {
            case A_POS:
               t->assocs[i].pos = read_s(ctx->file);
               break;
            case A_NAMED:
               t->assocs[i].name = tree_read(ctx);
               break;
            case A_RANGE:
               t->assocs[i].range.kind  = read_s(ctx->file);
               t->assocs[i].range.left  = tree_read(ctx);
               t->assocs[i].range.right = tree_read(ctx);
               break;
            case A_OTHERS:
               break;
            default:
               abort();
            }
         }
      }
      break;

   case T_ASSERT:
      t->severity = tree_read(ctx);
      t->message  = tree_read(ctx);
      break;

   case T_ENUM_LIT:
      t->pos = read_u(ctx->file);
      break;

   case T_INSTANCE:
      read_p(&t->genmaps, ctx);
      break;

   case T_IF:
      read_a(&t->elses, ctx);
      break;

   default:
      break;
   }

   t->n_attrs = read_s(ctx->file);
   assert(t->n_attrs <= MAX_ATTRS);
   t->attrs = xmalloc(sizeof(struct attr) * MAX_ATTRS);

   for (unsigned i = 0; i < t->n_attrs; i++) {
      t->attrs[i].kind = read_s(ctx->file);
      t->attrs[i].name = ident_read(ctx->file);

      switch (t->attrs[i].kind) {
      case A_STRING:
         {
            size_t len = read_s(ctx->file);
            t->attrs[i].sval = xmalloc(len + 1);
            fread(t->attrs[i].sval, len, 1, ctx->file);
            t->attrs[i].sval[len] = '\0';
         }
         break;

      case A_INT:
         t->attrs[i].ival = read_i(ctx->file);
         break;

      case A_TREE:
         t->attrs[i].tval = tree_read(ctx);
         break;

      default:
         abort();
      }
   }

#ifdef EXTRA_READ_CHECKS
   unsigned short term = read_s(ctx->file);
   if (term != 0xdead)
      fatal("bad tree termination marker %x kind=%d",
            term, t->kind);
#endif  // EXTRA_READ_CHECKS

   return t;
}

tree_rd_ctx_t tree_read_begin(FILE *f)
{
   struct tree_rd_ctx *ctx = xmalloc(sizeof(struct tree_rd_ctx));
   ctx->file     = f;
   ctx->type_ctx = type_read_begin(ctx);
   ctx->store_sz = 128;
   ctx->store    = xmalloc(ctx->store_sz * sizeof(tree_t));
   ctx->n_trees  = 0;

   return ctx;
}

void tree_read_end(tree_rd_ctx_t ctx)
{
   fclose(ctx->file);
   type_read_end(ctx->type_ctx);
   free(ctx->store);
   free(ctx);
}

FILE *tree_read_file(tree_rd_ctx_t ctx)
{
   return ctx->file;
}

tree_t tree_read_recall(tree_rd_ctx_t ctx, uint32_t index)
{
   assert(index < ctx->n_trees);
   return ctx->store[index];
}

static struct attr *tree_find_attr(tree_t t, ident_t name, attr_kind_t kind)
{
   assert(t != NULL);

   for (unsigned i = 0; i < t->n_attrs; i++) {
      if (t->attrs[i].kind == kind && t->attrs[i].name == name)
         return &t->attrs[i];
   }

   return NULL;
}

static struct attr *tree_add_attr(tree_t t, ident_t name, attr_kind_t kind)
{
   assert(t != NULL);
   assert(t->n_attrs < MAX_ATTRS);

   struct attr *a = tree_find_attr(t, name, kind);
   if (a != NULL)
      return a;

   if (t->attrs == NULL)
      t->attrs = xmalloc(sizeof(struct attr) * MAX_ATTRS);

   unsigned i = t->n_attrs++;
   t->attrs[i].kind = kind;
   t->attrs[i].name = name;

   return &t->attrs[i];
}

void tree_add_attr_str(tree_t t, ident_t name, const char *str)
{
   tree_add_attr(t, name, A_STRING)->sval = strdup(str);
}

const char *tree_attr_str(tree_t t, ident_t name)
{
   struct attr *a = tree_find_attr(t, name, A_STRING);
   return a ? a->sval : NULL;
}

void tree_add_attr_int(tree_t t, ident_t name, int n)
{
   tree_add_attr(t, name, A_INT)->ival = n;
}

int tree_attr_int(tree_t t, ident_t name, int def)
{
   struct attr *a = tree_find_attr(t, name, A_INT);
   return a ? a->ival : def;
}

void tree_add_attr_ptr(tree_t t, ident_t name, void *ptr)
{
   tree_add_attr(t, name, A_PTR)->pval = ptr;
}

void *tree_attr_ptr(tree_t t, ident_t name)
{
   struct attr *a = tree_find_attr(t, name, A_PTR);
   return a ? a->pval : NULL;
}

tree_t tree_attr_tree(tree_t t, ident_t name)
{
   struct attr *a = tree_find_attr(t, name, A_TREE);
   return a ? a->tval : NULL;
}

void tree_add_attr_tree(tree_t t, ident_t name, tree_t val)
{
   tree_add_attr(t, name, A_TREE)->tval = val;
}

int64_t assume_int(tree_t t)
{
   assert(tree_kind(t) == T_LITERAL);
   literal_t l = tree_literal(t);
   assert(l.kind == L_INT);
   return l.i;
}

void range_bounds(range_t r, int64_t *low, int64_t *high)
{
   int64_t left  = assume_int(r.left);
   int64_t right = assume_int(r.right);

   if (r.kind == RANGE_TO) {
      *low  = left;
      *high = right;
   }
   else {
      *low  = right;
      *high = left;
   }
}

static void rewrite_a(struct tree_array *a,
                      tree_rewrite_fn_t fn, void *context)
{
   for (unsigned i = 0; i < a->count; i++)
      a->items[i] = tree_rewrite(a->items[i], fn, context);

   // If an item was rewritten to NULL then delete it
   unsigned n = 0;
   for (unsigned i = 0; i < a->count; i++) {
      if (a->items[i] != NULL)
         a->items[n++] = a->items[i];
   }
   a->count = n;
}

static void rewrite_p(struct param_array *a,
                      tree_rewrite_fn_t fn, void *context)
{
   for (unsigned i = 0; i < a->count; i++) {
      switch (a->items[i].kind) {
      case P_RANGE:
         a->items[i].range.left =
            tree_rewrite(a->items[i].range.left, fn, context);
         a->items[i].range.right =
            tree_rewrite(a->items[i].range.right, fn, context);
         break;

      case P_POS:
      case P_NAMED:
         a->items[i].value =
            tree_rewrite(a->items[i].value, fn, context);
         break;
      }
   }
}

tree_t tree_rewrite(tree_t t, tree_rewrite_fn_t fn, void *context)
{
   if (HAS_GENERICS(t))
      rewrite_a(&t->generics, fn, context);
   if (HAS_PORTS(t))
      rewrite_a(&t->ports, fn, context);
   if (HAS_DECLS(t))
      rewrite_a(&t->decls, fn, context);
   if (HAS_TRIGGERS(t))
      rewrite_a(&t->triggers, fn, context);
   if (HAS_STMTS(t))
      rewrite_a(&t->stmts, fn, context);
   if (HAS_WAVEFORMS(t))
      rewrite_a(&t->waves, fn, context);
   if (HAS_TARGET(t))
      tree_set_target(t, tree_rewrite(tree_target(t), fn, context));
   if (HAS_VALUE(t)) {
      if (tree_has_value(t))
         tree_set_value(t, tree_rewrite(tree_value(t), fn, context));
   }
   if (HAS_DELAY(t)) {
      if (tree_has_delay(t))
         tree_set_delay(t, tree_rewrite(tree_delay(t), fn, context));
   }
   if (HAS_PARAMS(t))
      rewrite_p(&t->params, fn, context);
   if (HAS_RANGE(t)) {
      range_t r = tree_range(t);
      r.left  = tree_rewrite(r.left, fn, context);
      r.right = tree_rewrite(r.right, fn, context);
      tree_set_range(t, r);
   }

   switch (tree_kind(t)) {
   case T_ASSERT:
      tree_set_severity(t, tree_rewrite(tree_severity(t), fn, context));
      tree_set_message(t, tree_rewrite(tree_message(t), fn, context));
      break;

   case T_INSTANCE:
      rewrite_p(&t->genmaps, fn, context);
      break;

   case T_AGGREGATE:
      for (unsigned i = 0; i < tree_assocs(t); i++) {
         assoc_t *a = &t->assocs[i];
         a->value = tree_rewrite(a->value, fn, context);

         switch (a->kind) {
         case A_POS:
         case A_OTHERS:
            break;
         case A_NAMED:
            a->name = tree_rewrite(a->name, fn, context);
            break;
         case A_RANGE:
            a->range.left  = tree_rewrite(a->range.left, fn, context);
            a->range.right = tree_rewrite(a->range.right, fn, context);
            break;
         }
      }
      break;

   case T_TYPE_DECL:
      {
         type_t type = tree_type(t);

         switch (type_kind(type)) {
         case T_INTEGER:
         case T_PHYSICAL:
         case T_CARRAY:
            {
               for (unsigned i = 0; i < type_dims(type); i++) {
                  range_t r = type_dim(type, i);
                  r.left  = tree_rewrite(r.left, fn, context);
                  r.right = tree_rewrite(r.right, fn, context);
                  type_change_dim(type, i, r);
               }
            }
            break;

         default:
            break;
         }
      }
      break;

   case T_IF:
      rewrite_a(&t->elses, fn, context);
      break;

   default:
      break;
   }

   return (*fn)(t, context);
}

struct tree_copy_ctx {
   tree_t   *copied;
   size_t   n_copied;
   unsigned generation;
};

static tree_t tree_copy_aux(tree_t t, struct tree_copy_ctx *ctx);

static void copy_a(struct tree_array *from, struct tree_array *to,
                   struct tree_copy_ctx *ctx)
{
   to->count = to->max = from->count;
   to->items = xmalloc(to->count * sizeof(param_t));

   for (unsigned i = 0; i < from->count; i++)
      to->items[i] = tree_copy_aux(from->items[i], ctx);
}

static void copy_p(struct param_array *from, struct param_array *to,
                   struct tree_copy_ctx *ctx)
{
   to->count = to->max = from->count;
   to->items = xmalloc(to->count * sizeof(param_t));

   for (unsigned i = 0; i < from->count; i++) {
      param_t *fp = &from->items[i];
      param_t *tp = &to->items[i];

      switch ((tp->kind = fp->kind)) {
      case P_POS:
         tp->pos   = fp->pos;
         tp->value = tree_copy_aux(fp->value, ctx);
         break;
      case P_RANGE:
         tp->range.kind  = fp->range.kind;
         tp->range.left  = tree_copy_aux(fp->range.left, ctx);
         tp->range.right = tree_copy_aux(fp->range.right, ctx);
         break;
      case P_NAMED:
         tp->name  = fp->name;
         tp->value = tree_copy_aux(fp->value, ctx);
         break;
      }
   }
}

static tree_t tree_copy_aux(tree_t t, struct tree_copy_ctx *ctx)
{
   if (t == NULL)
      return NULL;

   if (t->generation == ctx->generation) {
      // Already copied this tree
      assert(t->index < ctx->n_copied);
      return ctx->copied[t->index];
   }

   tree_t copy = tree_new(t->kind);

   t->generation = ctx->generation;
   t->index      = (ctx->n_copied)++;
   ctx->copied[t->index] = copy;

   copy->loc        = t->loc;
   if (HAS_IDENT(t))
      copy->ident = t->ident;
   if (HAS_IDENT2(t))
      copy->ident2 = t->ident2;
   if (HAS_PORTS(t))
      copy_a(&t->ports, &copy->ports, ctx);
   if (HAS_GENERICS(t))
      copy_a(&t->generics, &copy->generics, ctx);
   if (HAS_DECLS(t))
      copy_a(&t->decls, &copy->decls, ctx);
   if (HAS_TRIGGERS(t))
      copy_a(&t->triggers, &copy->triggers, ctx);
   if (HAS_STMTS(t))
      copy_a(&t->stmts, &copy->stmts, ctx);
   if (HAS_WAVEFORMS(t))
      copy_a(&t->waves, &copy->waves, ctx);
   if (HAS_TYPE(t)) {
      copy->type = t->type;
      type_ref(copy->type);
   }
   if (HAS_VALUE(t))
      copy->value = tree_copy_aux(t->value, ctx);
   if (HAS_DELAY(t))
      copy->delay = tree_copy_aux(t->delay, ctx);
   if (HAS_TARGET(t))
      copy->target = tree_copy_aux(t->target, ctx);
   if (HAS_REF(t))
      copy->ref = tree_copy_aux(t->ref, ctx);
   if (HAS_CONTEXT(t)) {
      for (unsigned i = 0; i < tree_contexts(t); i++)
         tree_add_context(copy, tree_context(t, i));
   }
   if (HAS_PARAMS(t))
      copy_p(&t->params, &copy->params, ctx);
   if (HAS_RANGE(t)) {
      copy->range.kind  = t->range.kind;
      copy->range.left  = tree_copy_aux(t->range.left, ctx);
      copy->range.right = tree_copy_aux(t->range.right, ctx);
   }

   switch (t->kind) {
   case T_PORT_DECL:
      copy->port_mode = t->port_mode;
      break;

   case T_LITERAL:
      copy->literal = t->literal;
      break;

   case T_AGGREGATE:
      for (unsigned i = 0; i < tree_assocs(t); i++) {
         assoc_t a = tree_assoc(t, i);
         switch (a.kind) {
         case A_POS:
         case A_OTHERS:
            break;
         case A_NAMED:
            a.name = tree_copy_aux(a.name, ctx);
            break;
         case A_RANGE:
            a.range.left  = tree_copy_aux(a.range.left, ctx);
            a.range.right = tree_copy_aux(a.range.right, ctx);
            break;
         }

         tree_add_assoc(copy, a);
      }
      break;

   case T_ASSERT:
      copy->severity = tree_copy_aux(t->severity, ctx);
      copy->message  = tree_copy_aux(t->message, ctx);
      break;

   case T_ENUM_LIT:
      copy->pos = t->pos;
      break;

   case T_INSTANCE:
      copy_p(&t->genmaps, &copy->genmaps, ctx);
      break;

   case T_IF:
      copy_a(&t->elses, &copy->elses, ctx);
      break;

   default:
      break;
   }

   for (unsigned i = 0; i < t->n_attrs; i++) {
      switch (t->attrs[i].kind) {
      case A_STRING:
         tree_add_attr_str(copy, t->attrs[i].name, t->attrs[i].sval);
         break;

      case A_INT:
         tree_add_attr_int(copy, t->attrs[i].name, t->attrs[i].ival);
         break;

      case A_TREE:
         tree_add_attr_tree(copy, t->attrs[i].name, t->attrs[i].tval);
         break;

      case A_PTR:
         tree_add_attr_ptr(copy, t->attrs[i].name, t->attrs[i].pval);
         break;
      }
   }

   return copy;
}

tree_t tree_copy(tree_t t)
{
   struct tree_copy_ctx ctx = {
      .copied     = xmalloc(sizeof(tree_t) * n_trees_alloc),
      .n_copied   = 0,
      .generation = next_generation++
   };
   tree_t copy = tree_copy_aux(t, &ctx);
   free(ctx.copied);
   return copy;
}

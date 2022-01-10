//
//  Copyright (C) 2011-2022  Nick Gasson
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

#include "phase.h"
#include "util.h"
#include "common.h"
#include "loc.h"
#include "exec.h"
#include "hash.h"
#include "vcode.h"
#include "type.h"

#include <assert.h>
#include <string.h>
#include <stdarg.h>
#include <inttypes.h>
#include <stdlib.h>

#define MAX_BUILTIN_ARGS 2

typedef struct imp_signal imp_signal_t;

struct imp_signal {
   imp_signal_t *next;
   tree_t        signal;
   tree_t        process;
};

typedef struct {
   imp_signal_t *imp_signals;
   tree_t        top;
   exec_t       *exec;
   tree_flags_t  eval_mask;
   hash_t       *generics;
   hash_t       *subprograms;
} simp_ctx_t;

static tree_t simp_tree(tree_t t, void *context);
static void simp_build_wait(tree_t wait, tree_t expr, bool all);

static tree_t simp_call_args(tree_t t)
{
   tree_t decl = tree_ref(t);

   const int nparams = tree_params(t);
   const int nports  = tree_ports(decl);

   // Replace named arguments with positional ones

   int last_pos = -1;
   for (int i = 0; i < nparams; i++) {
      if (tree_subkind(tree_param(t, i)) == P_POS)
         last_pos = i;
   }

   if (last_pos < nparams - 1) {
      tree_t new = tree_new(tree_kind(t));
      tree_set_loc(new, tree_loc(t));
      tree_set_ident(new, tree_ident(t));
      tree_set_ref(new, tree_ref(t));

      tree_kind_t kind = tree_kind(t);
      if (kind == T_FCALL || kind == T_PROT_FCALL) {
         tree_set_type(new, tree_type(t));
         tree_set_flag(new, tree_flags(t));
      }
      else if (kind == T_CPCALL)
         tree_set_ident2(new, tree_ident2(t));

      if ((kind == T_PROT_PCALL || kind == T_PROT_FCALL) && tree_has_name(t))
         tree_set_name(new, tree_name(t));

      for (int i = 0; i <= last_pos; i++) {
         tree_t port  = tree_port(decl, i);
         tree_t param = tree_param(t, i);
         tree_t value = tree_value(param);

         if (tree_kind(value) == T_OPEN)
            value = tree_value(port);

         add_param(new, value, P_POS, NULL);
      }

      for (int i = last_pos + 1; i < nports; i++) {
         tree_t port  = tree_port(decl, i);
         ident_t name = tree_ident(port);

         bool found = false;
         for (int j = last_pos + 1; (j < nparams) && !found; j++) {
            tree_t p = tree_param(t, j);
            assert(tree_subkind(p) == P_NAMED);

            tree_t ref = tree_name(p);
            assert(tree_kind(ref) == T_REF);

            if (name == tree_ident(ref)) {
               tree_t value = tree_value(p);

               if (tree_kind(value) == T_OPEN)
                  value = tree_value(port);

               add_param(new, value, P_POS, NULL);
               found = true;
            }
         }
         assert(found);
      }

      t = new;
   }

   return t;
}

static bool fold_not_possible(tree_t t, eval_flags_t flags, const char *why)
{
   if (flags & EVAL_WARN)
      warn_at(tree_loc(t), "%s prevents constant folding", why);

   return false;
}

static bool fold_possible(tree_t t, eval_flags_t flags)
{
   switch (tree_kind(t)) {
   case T_FCALL:
      {
         tree_t decl = tree_ref(t);
         const subprogram_kind_t kind = tree_subkind(decl);
         if (kind == S_USER && !(flags & EVAL_FCALL))
            return fold_not_possible(t, flags, "call to user defined function");
         else if (kind == S_FOREIGN)
            return fold_not_possible(t, flags, "call to foreign function");
         else if (tree_flags(decl) & TREE_F_IMPURE)
            return fold_not_possible(t, flags, "call to impure function");
         else if (!(tree_flags(t) & TREE_F_GLOBALLY_STATIC))
            return fold_not_possible(t, flags, "non-static expression");
         else if (kind != S_USER && !is_open_coded_builtin(kind)
                  && vcode_find_unit(tree_ident2(decl)) == NULL)
            return fold_not_possible(t, flags, "not yet lowered predef");

         const int nparams = tree_params(t);
         for (int i = 0; i < nparams; i++) {
            tree_t p = tree_value(tree_param(t, i));
            if (!fold_possible(p, flags))
               return false;
            else if (tree_kind(p) == T_FCALL && type_is_scalar(tree_type(p)))
               return false;  // Would have been folded already if possible
         }

         return true;
      }

   case T_LITERAL:
      return true;

   case T_TYPE_CONV:
      return fold_possible(tree_value(t), flags);

   case T_QUALIFIED:
      return fold_possible(tree_value(t), flags);

   case T_REF:
      {
         tree_t decl = tree_ref(t);
         switch (tree_kind(decl)) {
         case T_UNIT_DECL:
         case T_ENUM_LIT:
            return true;

         case T_CONST_DECL:
            if (tree_has_value(decl))
               return fold_possible(tree_value(decl), flags);
            else if (!(flags & EVAL_FCALL))
               return fold_not_possible(t, flags, "deferred constant");
            else
               return true;

         default:
            return fold_not_possible(t, flags, "reference");
         }
      }

   case T_RECORD_REF:
      return fold_possible(tree_value(t), flags);

   case T_AGGREGATE:
      {
         const int nassocs = tree_assocs(t);
         for (int i = 0; i < nassocs; i++) {
            if (!fold_possible(tree_value(tree_assoc(t, i)), flags))
               return false;
         }

         return true;
      }

   default:
      return fold_not_possible(t, flags, "aggregate");
   }
}

static tree_t simp_fold(tree_t t, simp_ctx_t *ctx)
{
   type_t type = tree_type(t);
   if (!type_is_scalar(type))
      return t;
   else if (!fold_possible(t, exec_get_flags(ctx->exec)))
      return t;

   vcode_unit_t thunk = lower_thunk(t);
   if (thunk == NULL)
      return t;

   tree_t folded = exec_fold(ctx->exec, t, thunk);

   vcode_unit_unref(thunk);
   thunk = NULL;

   return folded;
}

static vcode_unit_t simp_lower_cb(ident_t func, void *__ctx)
{
   simp_ctx_t *ctx = __ctx;

   tree_t decl = hash_get(ctx->subprograms, func);
   if (decl == NULL)
      return NULL;

   return lower_thunk(decl);
}

static tree_t simp_fcall(tree_t t, simp_ctx_t *ctx)
{
   t = simp_call_args(t);

   if (tree_flags(t) & ctx->eval_mask)
      return simp_fold(t, ctx);

   return t;
}

static tree_t simp_type_conv(tree_t t, simp_ctx_t *ctx)
{
   return simp_fold(t, ctx);
}

static tree_t simp_pcall(tree_t t)
{
   return simp_call_args(t);
}

static tree_t simp_record_ref(tree_t t)
{
   tree_t value = tree_value(t), agg = NULL;
   switch (tree_kind(value)) {
   case T_AGGREGATE:
      agg = value;
      break;

   case T_REF:
      {
         tree_t decl = tree_ref(value);
         if (tree_kind(decl) != T_CONST_DECL)
            return t;
         else if (!tree_has_value(decl))
            return t;

         agg = tree_value(decl);
         if (tree_kind(agg) != T_AGGREGATE)
            return t;
      }
      break;

   case T_OPEN:
      return value;

   default:
      return t;
   }

   ident_t field = tree_ident(t);
   type_t type = tree_type(agg);

   const int nassocs = tree_assocs(agg);
   for (int i = 0; i < nassocs; i++) {
      tree_t a = tree_assoc(agg, i);
      switch (tree_subkind(a)) {
      case A_POS:
         if (tree_ident(type_field(type, tree_pos(a))) == field)
            return tree_value(a);
         break;

      case A_NAMED:
         if (tree_ident(tree_name(a)) == field)
            return tree_value(a);
         break;
      }
   }

   return t;
}

static tree_t simp_ref(tree_t t, simp_ctx_t *ctx)
{
   if (tree_flags(t) & TREE_F_FORMAL_NAME)
      return t;

   tree_t decl = tree_ref(t);

   switch (tree_kind(decl)) {
   case T_CONST_DECL:
      if (!type_is_scalar(tree_type(decl)))
         return t;
      else if (tree_has_value(decl)) {
         tree_t value = tree_value(decl);
         switch (tree_kind(value)) {
         case T_LITERAL:
            return value;

         case T_REF:
            if (tree_kind(tree_ref(value)) == T_ENUM_LIT)
               return value;
            // Fall-through

         default:
            return t;
         }
      }
      else
         return t;

   case T_UNIT_DECL:
      return tree_value(decl);

   case T_PORT_DECL:
      if (ctx->generics != NULL) {
         tree_t map = hash_get(ctx->generics, decl);
         if (map != NULL) {
            switch (tree_kind(map)) {
            case T_LITERAL:
            case T_AGGREGATE:
            case T_ARRAY_SLICE:
            case T_ARRAY_REF:
            case T_FCALL:
            case T_RECORD_REF:
            case T_OPEN:
            case T_QUALIFIED:
               // Do not rewrite references to non-references if they appear
               // as formal names
               if (tree_flags(t) & TREE_F_FORMAL_NAME)
                  break;
               // Fall-through
            case T_REF:
               return map;
            default:
               fatal_trace("cannot rewrite generic %s to tree kind %s",
                           istr(tree_ident(t)), tree_kind_str(tree_kind(map)));
            }
         }
      }
      return t;

   default:
      return t;
   }
}

static tree_t simp_attr_delayed_transaction(tree_t t, attr_kind_t predef,
                                            simp_ctx_t *ctx)
{
   tree_t name = tree_name(t);
   assert(tree_kind(name) == T_REF);

   tree_t decl = tree_ref(name);

   const tree_kind_t kind = tree_kind(decl);
   if (kind != T_SIGNAL_DECL && kind != T_PORT_DECL)
      return t;

   char *sig_name LOCAL =
      xasprintf("%s_%s", (predef == ATTR_DELAYED) ? "delayed" : "transaction",
                istr(tree_ident(name)));

   tree_t s = tree_new(T_SIGNAL_DECL);
   tree_set_loc(s, tree_loc(t));
   tree_set_ident(s, ident_uniq(sig_name));
   tree_set_type(s, tree_type(t));

   tree_t p = tree_new(T_PROCESS);
   tree_set_loc(p, tree_loc(t));
   tree_set_ident(p, ident_prefix(tree_ident(s), ident_new("p"), '_'));

   tree_t r = make_ref(s);

   tree_t a = tree_new(T_SIGNAL_ASSIGN);
   tree_set_ident(a, ident_new("assign"));
   tree_set_target(a, r);

   switch (predef) {
   case ATTR_DELAYED:
      {
         if (tree_has_value(decl))
            tree_set_value(s, tree_value(decl));
         else
            tree_set_value(s, make_default_value(tree_type(t), tree_loc(t)));

         tree_t delay = tree_value(tree_param(t, 0));

         tree_t wave = tree_new(T_WAVEFORM);
         tree_set_value(wave, name);
         tree_set_delay(wave, delay);

         tree_add_waveform(a, wave);
      }
      break;

   case ATTR_TRANSACTION:
      {
         tree_set_value(s, make_default_value(tree_type(s), tree_loc(s)));

         tree_t not_decl = std_func(ident_new("STD.STANDARD.\"not\"(B)B"));
         assert(not_decl != NULL);

         tree_t not = tree_new(T_FCALL);
         tree_set_ident(not, ident_new("\"not\""));
         tree_set_ref(not, not_decl);
         tree_set_type(not, type_result(tree_type(not_decl)));
         add_param(not, r, P_POS, NULL);

         tree_t wave = tree_new(T_WAVEFORM);
         tree_set_value(wave, not);

         tree_add_waveform(a, wave);
      }
      break;

   default:
      break;
   }

   tree_add_stmt(p, a);

   tree_t wait = tree_new(T_WAIT);
   tree_set_ident(wait, ident_new("wait"));
   tree_set_flag(wait, TREE_F_STATIC_WAIT);
   tree_add_trigger(wait, name);

   tree_add_stmt(p, wait);

   imp_signal_t *imp = xmalloc(sizeof(imp_signal_t));
   imp->next    = ctx->imp_signals;
   imp->signal  = s;
   imp->process = p;

   ctx->imp_signals = imp;

   return r;
}

static tree_t simp_attr_ref(tree_t t, simp_ctx_t *ctx)
{
   if (tree_has_value(t))
      return tree_value(t);

   const attr_kind_t predef = tree_subkind(t);
   switch (predef) {
   case ATTR_DELAYED:
   case ATTR_TRANSACTION:
      return simp_attr_delayed_transaction(t, predef, ctx);

   case ATTR_POS:
      {
         int64_t arg;
         if (folded_int(tree_value(tree_param(t, 0)), &arg))
            return get_int_lit(t, NULL, arg);
         else
            return t;
      }

   case ATTR_LENGTH:
   case ATTR_LEFT:
   case ATTR_LOW:
   case ATTR_HIGH:
   case ATTR_RIGHT:
   case ATTR_ASCENDING:
      {
         tree_t name = tree_name(t);
         const tree_kind_t name_kind = tree_kind(name);

         if (name_kind != T_REF
             && !(name_kind == T_ATTR_REF && tree_subkind(name) == ATTR_BASE))
            return t;   // Cannot fold this

         type_t type = tree_type(name);
         int64_t dim_i = 1;

         if (type_kind(type) == T_ENUM) {
            // Enumeration subtypes are handled below
            const int nlits = type_enum_literals(type);

            switch (predef) {
            case ATTR_LEFT:
            case ATTR_LOW:
               return make_ref(type_enum_literal(type, 0));
            case ATTR_RIGHT:
            case ATTR_HIGH:
               return make_ref(type_enum_literal(type, nlits - 1));
            case ATTR_ASCENDING:
               return get_enum_lit(t, NULL, true);
            default:
               fatal_trace("invalid enumeration attribute %d", predef);
            }
         }

         if (type_is_array(type)) {
            if (tree_params(t) > 0) {
               tree_t value = tree_value(tree_param(t, 0));
               if (!folded_int(value, &dim_i))
                  fatal_at(tree_loc(value), "locally static dimension "
                           "expression was not folded");
            }

            if (name_kind == T_REF
                && tree_kind(tree_ref(name)) == T_TYPE_DECL
                && type_is_unconstrained(type)) {

               // Get index type of unconstrained array

               if (dim_i < 1 || dim_i > type_index_constrs(type))
                  return t;

               type  = type_index_constr(type, dim_i - 1);
               dim_i = 1;
            }
            else if (type_is_unconstrained(type))
               return t;
            else if (dim_i < 1 || dim_i > dimension_of(type))
               return t;
         }

         tree_t r = range_of(type, dim_i - 1);

         const range_kind_t rkind = tree_subkind(r);
         if (rkind != RANGE_TO && rkind != RANGE_DOWNTO)
            return t;

         switch (predef) {
         case ATTR_LENGTH:
            if (tree_kind(tree_left(r)) == T_LITERAL
                && tree_kind(tree_right(r)) == T_LITERAL) {
               int64_t low, high;
               range_bounds(r, &low, &high);
               return get_int_lit(t, NULL, (high < low) ? 0 : high - low + 1);
            }
            else
               return t;

         case ATTR_LOW:
            return (rkind == RANGE_TO) ? tree_left(r) : tree_right(r);
         case ATTR_HIGH:
            return (rkind == RANGE_TO) ? tree_right(r) : tree_left(r);
         case ATTR_LEFT:
            return tree_left(r);
         case ATTR_RIGHT:
            return tree_right(r);
         case ATTR_ASCENDING:
            return get_enum_lit(t, NULL, (rkind == RANGE_TO));
         default:
            return t;
         }
      }

   default:
      return t;
   }
}

static tree_t simp_extract_string_literal(tree_t literal, int64_t index,
                                          tree_t def)
{
   type_t type = tree_type(literal);
   if (type_is_unconstrained(type))
      return def;

   tree_t bounds = range_of(type, 0);
   int64_t low, high;
   range_bounds(bounds, &low, &high);

   const bool to = (tree_subkind(bounds) == RANGE_TO);

   const int pos = to ? (index + low) : (high - index);
   if ((pos < 0) || (pos > tree_chars(literal)))
      return def;

   return tree_char(literal, pos);
}

static tree_t simp_extract_aggregate(tree_t agg, int64_t index, tree_t def)
{
   type_t type = tree_type(agg);
   if (type_is_unconstrained(type))
      return def;

   tree_t bounds = range_of(tree_type(agg), 0);
   int64_t low, high;
   range_bounds(bounds, &low, &high);

   const bool to = (tree_subkind(bounds) == RANGE_TO);

   const int nassocs = tree_assocs(agg);
   for (int i = 0; i < nassocs; i++) {
      tree_t a = tree_assoc(agg, i);
      switch (tree_subkind(a)) {
      case A_POS:
         {
            const int pos = tree_pos(a);
            if ((to && (pos + low == index))
                || (!to && (high - pos == index)))
               return tree_value(a);
         }
         break;

      case A_OTHERS:
         return tree_value(a);

      case A_RANGE:
         {
            tree_t r = tree_range(a, 0);
            const int64_t left  = assume_int(tree_left(r));
            const int64_t right = assume_int(tree_right(r));

            if ((to && (index >= left) && (index <= right))
                || (!to && (index <= left) && (index >= right)))
               return tree_value(a);
         }
         break;

      case A_NAMED:
         if (assume_int(tree_name(a)) == index)
            return tree_value(a);
         break;
      }
   }

   return def;
}

static tree_t simp_array_slice(tree_t t)
{
   tree_t value = tree_value(t);

   if (tree_kind(value) == T_OPEN)
      return value;

   return t;
}

static tree_t simp_array_ref(tree_t t)
{
   tree_t value = tree_value(t);

   if (tree_kind(value) == T_OPEN)
      return value;

   const int nparams = tree_params(t);

   int64_t indexes[nparams];
   bool can_fold = true;
   for (int i = 0; i < nparams; i++) {
      tree_t p = tree_param(t, i);
      assert(tree_subkind(p) == P_POS);
      can_fold = can_fold && folded_int(tree_value(p), &indexes[i]);
   }

   if (!can_fold)
      return t;

   if (!tree_has_type(value))
      return t;

   const tree_kind_t value_kind = tree_kind(value);
   if (value_kind == T_AGGREGATE)
      return simp_extract_aggregate(value, indexes[0], t);
   else if (value_kind == T_LITERAL)
      return simp_extract_string_literal(value, indexes[0], t);
   else if (value_kind != T_REF)
      return t;   // Cannot fold nested array references

   tree_t decl = tree_ref(value);

   if (nparams > 1)
      return t;  // Cannot constant fold multi-dimensional arrays

   assert(nparams == 1);

   switch (tree_kind(decl)) {
   case T_CONST_DECL:
      {
         if (!tree_has_value(decl))
            return t;

         tree_t v = tree_value(decl);
         if (tree_kind(v) != T_AGGREGATE)
            return t;

         return simp_extract_aggregate(v, indexes[0], t);
      }
   default:
      return t;
   }
}

static tree_t simp_process(tree_t t)
{
   // Replace sensitivity list with a "wait on" statement
   const int ntriggers = tree_triggers(t);
   if (ntriggers > 0) {
      const int nstmts = tree_stmts(t);
      if (nstmts == 0)
         return NULL;   // Body was optimised away

      tree_t p = tree_new(T_PROCESS);
      tree_set_ident(p, tree_ident(t));
      tree_set_loc(p, tree_loc(t));

      const int ndecls = tree_decls(t);
      for (int i = 0; i < ndecls; i++)
         tree_add_decl(p, tree_decl(t, i));

      for (int i = 0; i < nstmts; i++)
         tree_add_stmt(p, tree_stmt(t, i));

      tree_t w = tree_new(T_WAIT);
      tree_set_ident(w, tree_ident(p));
      tree_set_flag(w, TREE_F_STATIC_WAIT);
      if (ntriggers == 1 && tree_kind(tree_trigger(t, 0)) == T_ALL)
         simp_build_wait(w, t, true);
      else {
         for (int i = 0; i < ntriggers; i++)
            tree_add_trigger(w, tree_trigger(t, i));
      }
      tree_add_stmt(p, w);

      return p;
   }

   // Delete processes that contain just a single wait statement
   if (tree_stmts(t) == 1 && tree_kind(tree_stmt(t, 0)) == T_WAIT)
      return NULL;
   else
      return t;
}

static tree_t simp_wait(tree_t t)
{
   // LRM 93 section 8.1
   // If there is no sensitivity list supplied generate one from the
   // condition clause

   if (tree_has_value(t) && tree_triggers(t) == 0)
      simp_build_wait(t, tree_value(t), false);

   return t;
}

static tree_t simp_case(tree_t t)
{
   const int nassocs = tree_assocs(t);
   if (nassocs == 0)
      return NULL;    // All choices are unreachable

   int64_t ival;
   if (folded_int(tree_value(t), &ival)) {
      for (int i = 0; i < nassocs; i++) {
         tree_t a = tree_assoc(t, i);
         switch ((assoc_kind_t)tree_subkind(a)) {
         case A_NAMED:
            {
               int64_t aval;
               if (folded_int(tree_name(a), &aval) && (ival == aval)) {
                  if (tree_has_value(a))
                     return tree_value(a);
                  else
                     return NULL;
               }
            }
            break;

         case A_RANGE:
            continue;   // TODO

         case A_OTHERS:
            if (tree_has_value(a))
               return tree_value(a);
            else
               return NULL;

         case A_POS:
            break;
         }
      }
   }

   return t;
}

static tree_t simp_if(tree_t t)
{
   bool value_b;
   if (folded_bool(tree_value(t), &value_b)) {
      if (value_b) {
         // If statement always executes so replace with then part
         if (tree_stmts(t) == 1)
            return tree_stmt(t, 0);
         else {
            tree_t b = tree_new(T_BLOCK);
            tree_set_loc(b, tree_loc(t));
            tree_set_ident(b, tree_ident(t));
            for (unsigned i = 0; i < tree_stmts(t); i++)
               tree_add_stmt(b, tree_stmt(t, i));
            return b;
         }
      }
      else {
         // If statement never executes so replace with else part
         if (tree_else_stmts(t) == 1)
            return tree_else_stmt(t, 0);
         else if (tree_else_stmts(t) == 0)
            return NULL;   // Delete it
         else {
            tree_t b = tree_new(T_BLOCK);
            tree_set_loc(b, tree_loc(t));
            tree_set_ident(b, tree_ident(t));
            for (unsigned i = 0; i < tree_else_stmts(t); i++)
               tree_add_stmt(b, tree_else_stmt(t, i));
            return b;
         }
      }
   }
   else
      return t;
}

static tree_t simp_while(tree_t t)
{
   bool value_b;
   if (!tree_has_value(t))
      return t;
   else if (folded_bool(tree_value(t), &value_b) && !value_b) {
      // Condition is false so loop never executes
      return NULL;
   }
   else
      return t;
}

static bool simp_is_static(tree_t expr)
{
   switch (tree_kind(expr)) {
   case T_REF:
      {
         tree_t decl = tree_ref(expr);
         switch (tree_kind(decl)) {
         case T_CONST_DECL:
         case T_UNIT_DECL:
         case T_ENUM_LIT:
            return true;
         case T_PORT_DECL:
            return tree_class(decl) == C_CONSTANT;
         case T_ALIAS:
            return simp_is_static(tree_value(decl));
         default:
            return false;
         }
      }

   case T_LITERAL:
      return true;

   default:
      return false;
   }
}

static tree_t simp_longest_static_prefix(tree_t expr)
{
   switch (tree_kind(expr)) {
   case T_ARRAY_REF:
      {
         tree_t value = tree_value(expr);
         tree_t prefix = simp_longest_static_prefix(tree_value(expr));

         if (prefix != value)
            return prefix;

         const int nparams = tree_params(expr);
         for (int i = 0; i < nparams; i++) {
            if (!simp_is_static(tree_value(tree_param(expr, i))))
               return prefix;
         }

         return expr;
      }

   case T_ARRAY_SLICE:
      {
         tree_t value = tree_value(expr);
         tree_t prefix = simp_longest_static_prefix(tree_value(expr));

         if (prefix != value)
            return prefix;

         const int nranges = tree_ranges(expr);
         for (int i = 0; i < nranges; i++) {
            tree_t r = tree_range(expr, i);
            if (!simp_is_static(tree_left(r)) || !simp_is_static(tree_right(r)))
               return prefix;
         }

         return expr;
      }

   default:
      return expr;
   }
}

static void simp_build_wait_for_target(tree_t wait, tree_t expr, bool all)
{
   switch (tree_kind(expr)) {
   case T_ARRAY_SLICE:
      simp_build_wait(wait, tree_range(expr, 0), all);
      break;

   case T_ARRAY_REF:
      {
         const int nparams = tree_params(expr);
         for (int i = 0; i < nparams; i++)
            simp_build_wait(wait, tree_value(tree_param(expr, i)), all);
      }
      break;

   default:
      break;
   }
}

static void simp_build_wait(tree_t wait, tree_t expr, bool all)
{
   // LRM 08 section 10.2 has rules for building a wait statement from a
   // sensitivity list. LRM 08 section 11.3 extends these rules to
   // all-sensitised processes.

   switch (tree_kind(expr)) {
   case T_REF:
      {
         tree_t decl = tree_ref(expr);
         if (class_of(decl) == C_SIGNAL) {
            // Check for duplicates
            const int ntriggers = tree_triggers(wait);
            for (int i = 0; i < ntriggers; i++) {
               tree_t t = tree_trigger(wait, i);
               if (tree_kind(t) == T_REF && tree_ref(t) == decl)
                  return;
            }

            tree_add_trigger(wait, expr);
         }
      }
      break;

   case T_ARRAY_SLICE:
      if (class_of(expr) == C_SIGNAL) {
         if (simp_longest_static_prefix(expr) == expr)
            tree_add_trigger(wait, expr);
         else {
            simp_build_wait(wait, tree_value(expr), all);
            simp_build_wait_for_target(wait, expr, all);
         }
      }
      break;

   case T_WAVEFORM:
   case T_RECORD_REF:
   case T_QUALIFIED:
   case T_TYPE_CONV:
   case T_ASSERT:
      if (tree_has_value(expr))
         simp_build_wait(wait, tree_value(expr), all);
      break;

   case T_ARRAY_REF:
      if (class_of(expr) == C_SIGNAL) {
         if (simp_longest_static_prefix(expr) == expr)
            tree_add_trigger(wait, expr);
         else {
            simp_build_wait(wait, tree_value(expr), all);
            simp_build_wait_for_target(wait, expr, all);
         }
      }
      break;

   case T_FCALL:
   case T_PCALL:
      {
         tree_t decl = tree_ref(expr);
         const int nparams = tree_params(expr);
         const int nports = tree_ports(decl);
         for (int i = 0; i < nparams; i++) {
            port_mode_t mode = PORT_IN;
            if (i < nports)
               mode = tree_subkind(tree_port(decl, i));
            if (mode == PORT_IN || mode == PORT_INOUT)
               simp_build_wait(wait, tree_value(tree_param(expr, i)), all);
         }

         const tree_kind_t kind = tree_kind(decl);
         if (all && kind == T_PROC_BODY)
            simp_build_wait(wait, decl, all);
      }
      break;

   case T_AGGREGATE:
      {
         const int nassocs = tree_assocs(expr);
         for (int i = 0; i < nassocs; i++)
            simp_build_wait(wait, tree_value(tree_assoc(expr, i)), all);
      }
      break;

   case T_ATTR_REF:
      {
         const attr_kind_t predef = tree_subkind(expr);
         if (predef == ATTR_EVENT || predef == ATTR_ACTIVE)
            simp_build_wait(wait, tree_name(expr), all);

         const int nparams = tree_params(expr);
         for (int i = 0; i < nparams; i++)
            simp_build_wait(wait, tree_value(tree_param(expr, i)), all);
      }
      break;

   case T_LITERAL:
      break;

   case T_IF:
      {
         simp_build_wait(wait, tree_value(expr), all);

         const int nstmts = tree_stmts(expr);
         for (int i = 0; i < nstmts; i++)
            simp_build_wait(wait, tree_stmt(expr, i), all);

         const int nelses = tree_else_stmts(expr);
         for (int i = 0; i < nelses; i++)
            simp_build_wait(wait, tree_else_stmt(expr, i), all);
      }
      break;

   case T_PROCESS:
   case T_BLOCK:
   case T_PROC_BODY:
      {
         const int nstmts = tree_stmts(expr);
         for (int i = 0; i < nstmts; i++)
            simp_build_wait(wait, tree_stmt(expr, i), all);
      }
      break;

   case T_SIGNAL_ASSIGN:
      {
         simp_build_wait_for_target(wait, tree_target(expr), all);

         const int nwaves = tree_waveforms(expr);
         for (int i = 0; i < nwaves; i++)
            simp_build_wait(wait, tree_waveform(expr, i), all);
      }
      break;

   case T_VAR_ASSIGN:
      simp_build_wait_for_target(wait, tree_target(expr), all);
      simp_build_wait(wait, tree_value(expr), all);
      break;

   case T_CASE:
      {
         simp_build_wait(wait, tree_value(expr), all);

         const int nassocs = tree_assocs(expr);
         for (int i = 0; i < nassocs; i++)
            simp_build_wait(wait, tree_value(tree_assoc(expr, i)), all);
      }
      break;

   case T_FOR:
      {
         simp_build_wait(wait, tree_range(expr, 0), all);

         const int nstmts = tree_stmts(expr);
         for (int i = 0; i < nstmts; i++)
            simp_build_wait(wait, tree_stmt(expr, i), all);
      }
      break;

   case T_WHILE:
      {
         simp_build_wait(wait, tree_value(expr), all);

         const int nstmts = tree_stmts(expr);
         for (int i = 0; i < nstmts; i++)
            simp_build_wait(wait, tree_stmt(expr, i), all);
      }
      break;

   case T_RANGE:
      if (tree_subkind(expr) == RANGE_EXPR)
         simp_build_wait(wait, tree_value(expr), all);
      else {
         simp_build_wait(wait, tree_left(expr), all);
         simp_build_wait(wait, tree_right(expr), all);
      }
      break;

   default:
      fatal_trace("Cannot handle tree kind %s in wait expression",
                  tree_kind_str(tree_kind(expr)));
   }
}

static tree_t simp_guard(tree_t t, tree_t wait)
{
   // See LRM 93 section 9.3

   tree_t g_if = tree_new(T_IF);
   tree_set_ident(g_if, ident_new("guard_if"));
   tree_set_loc(g_if, tree_loc(t));

   tree_t guard_ref = tree_guard(t);
   tree_set_value(g_if, guard_ref);
   tree_add_trigger(wait, guard_ref);

   // TODO: handle disconnection specifications here

   return g_if;
}

static tree_t simp_cassign(tree_t t)
{
   // Replace concurrent assignments with a process

   tree_t p = tree_new(T_PROCESS);
   tree_set_ident(p, tree_ident(t));
   tree_set_loc(p, tree_loc(t));

   tree_t w = tree_new(T_WAIT);
   tree_set_ident(w, ident_new("cassign"));
   tree_set_flag(w, TREE_F_STATIC_WAIT);

   tree_t container = p;  // Where to add new statements
   void (*add_stmt)(tree_t, tree_t) = tree_add_stmt;

   if (tree_has_guard(t)) {
      container = simp_guard(t, w);
      tree_add_stmt(p, container);
   }

   tree_t target = tree_target(t);

   const int nconds = tree_conds(t);
   for (int i = 0; i < nconds; i++) {
      tree_t c = tree_cond(t, i);

      if (tree_has_value(c)) {
         // Replace this with an if statement
         tree_t i = tree_new(T_IF);
         tree_set_value(i, tree_value(c));
         tree_set_ident(i, ident_uniq("cond"));

         simp_build_wait(w, tree_value(c), false);

         (*add_stmt)(container, i);

         container = i;
         add_stmt  = tree_add_stmt;
      }

      tree_t s = tree_new(T_SIGNAL_ASSIGN);
      tree_set_loc(s, tree_loc(t));
      tree_set_target(s, target);
      tree_set_ident(s, tree_ident(t));
      if (tree_has_reject(c))
         tree_set_reject(s, tree_reject(c));

      const int nwaves = tree_waveforms(c);
      for (int i = 0; i < nwaves; i++) {
         tree_t wave = tree_waveform(c, i);
         tree_add_waveform(s, wave);
         simp_build_wait(w, wave, false);
      }

      (*add_stmt)(container, s);

      if (tree_has_value(c)) {
         // Add subsequent statements to the else part
         add_stmt = tree_add_else_stmt;
      }
   }

   tree_add_stmt(p, w);
   return p;
}

static tree_t simp_select(tree_t t)
{
   // Replace a select statement with a case inside a process

   tree_t p = tree_new(T_PROCESS);
   tree_set_ident(p, tree_ident(t));
   tree_set_loc(p, tree_loc(t));

   tree_t w = tree_new(T_WAIT);
   tree_set_ident(w, ident_new("select_wait"));
   tree_set_flag(w, TREE_F_STATIC_WAIT);

   tree_t container = p;
   if (tree_has_guard(t)) {
      container = simp_guard(t, w);
      tree_add_stmt(p, container);
   }

   tree_t c = tree_new(T_CASE);
   tree_set_ident(c, ident_new("select_case"));
   tree_set_loc(c, tree_loc(t));
   tree_set_value(c, tree_value(t));

   simp_build_wait(w, tree_value(t), false);

   const int nassocs = tree_assocs(t);
   for (int i = 0; i < nassocs; i++) {
      tree_t a = tree_assoc(t, i);
      tree_add_assoc(c, a);

      if (tree_subkind(a) == A_NAMED)
         simp_build_wait(w, tree_name(a), false);

      tree_t value = tree_value(a);

      const int nwaveforms = tree_waveforms(value);
      for (int j = 0; j < nwaveforms; j++)
         simp_build_wait(w, tree_waveform(value, j), false);
   }

   tree_add_stmt(container, c);
   tree_add_stmt(p, w);
   return p;
}

static tree_t simp_cpcall(tree_t t)
{
   t = simp_call_args(t);

   tree_t process = tree_new(T_PROCESS);
   tree_set_ident(process, tree_ident(t));
   tree_set_loc(process, tree_loc(t));

   tree_t wait = tree_new(T_WAIT);
   tree_set_ident(wait, ident_new("pcall_wait"));

   tree_t pcall = tree_new(T_PCALL);
   tree_set_ident(pcall, ident_new("pcall"));
   tree_set_ident2(pcall, tree_ident2(t));
   tree_set_loc(pcall, tree_loc(t));
   tree_set_ref(pcall, tree_ref(t));

   const int nparams = tree_params(t);
   for (int i = 0; i < nparams; i++) {
      tree_t p = tree_param(t, i);
      assert(tree_subkind(p) == P_POS);

      // Only add IN and INOUT parameters to sensitivity list
      tree_t port = tree_port(tree_ref(t), i);
      port_mode_t mode = tree_subkind(port);
      if (mode == PORT_IN || mode == PORT_INOUT)
         simp_build_wait(wait, tree_value(p), false);

      tree_add_param(pcall, p);
   }

   tree_add_stmt(process, pcall);
   tree_add_stmt(process, wait);

   return process;
}

static tree_t simp_cassert(tree_t t)
{
   tree_t value = tree_value(t);
   bool value_b;
   if (folded_bool(value, &value_b) && value_b) {
      // Assertion always passes
      return NULL;
   }

   tree_t process = tree_new(T_PROCESS);
   tree_set_ident(process, tree_ident(t));
   tree_set_loc(process, tree_loc(t));

   if (tree_flags(t) & TREE_F_POSTPONED)
      tree_set_flag(process, TREE_F_POSTPONED);

   tree_t wait = tree_new(T_WAIT);
   tree_set_ident(wait, ident_new("assert_wait"));
   tree_set_flag(wait, TREE_F_STATIC_WAIT);

   tree_t a = tree_new(T_ASSERT);
   tree_set_ident(a, ident_new("assert_wrap"));
   tree_set_loc(a, tree_loc(t));
   tree_set_value(a, value);
   tree_set_severity(a, tree_severity(t));
   if (tree_has_message(t))
      tree_set_message(a, tree_message(t));

   simp_build_wait(wait, tree_value(t), false);

   tree_add_stmt(process, a);
   tree_add_stmt(process, wait);

   return process;
}

static tree_t simp_context_ref(tree_t t, simp_ctx_t *ctx)
{
   tree_t decl = tree_ref(t);

   const int nctx = tree_contexts(decl);
   for (int i = 2; i < nctx; i++)
      tree_add_context(ctx->top, tree_context(decl, i));

   return NULL;
}

static tree_t simp_use(tree_t t)
{
   tree_t lib_decl = tree_ref(t);
   if (tree_kind(lib_decl) != T_LIBRARY)
      return t;

   ident_t qual = tree_ident(t);
   ident_t lalias = ident_until(qual, '.');
   ident_t lname = tree_ident2(lib_decl);

   if (lalias != lname) {
      ident_t rest = ident_from(qual, '.');
      tree_set_ident(t, ident_prefix(lname, rest, '.'));
   }

   return t;
}

static tree_t simp_assert(tree_t t)
{
   bool value_b;
   if (!tree_has_value(t))
      return t;
   else if (folded_bool(tree_value(t), &value_b) && value_b) {
      // Assertion always passes
      return NULL;
   }
   else
      return t;
}

static tree_t simp_if_generate(tree_t t)
{
   bool value_b;
   if (!folded_bool(tree_value(t), &value_b))
      return t;

   if (value_b) {
      tree_t block = tree_new(T_BLOCK);
      tree_set_ident(block, tree_ident(t));
      tree_set_loc(block, tree_loc(t));

      const int ndecls = tree_decls(t);
      for (int i = 0; i < ndecls; i++)
         tree_add_decl(block, tree_decl(t, i));

      const int nstmts = tree_stmts(t);
      for (int i = 0; i < nstmts; i++)
         tree_add_stmt(block, tree_stmt(t, i));

      return block;
   }
   else
      return NULL;
}

static tree_t simp_signal_assign(tree_t t)
{
   tree_t target = tree_target(t);

   if (tree_kind(target) == T_OPEN)
      return NULL;    // Delete it

   return t;
}

static tree_t simp_assoc(tree_t t)
{
   if (!tree_has_value(t))
      return NULL;   // Delete it

   return t;
}

static tree_t simp_literal(tree_t t)
{
   switch (tree_subkind(t)) {
   case L_PHYSICAL:
      // Rewrite in terms of the base unit
      if (tree_has_ref(t)) {
         tree_t decl = tree_ref(t);
         int64_t base = assume_int(tree_value(decl));

         // TODO: check for overflow here
         if (tree_ival(t) == 0)
            tree_set_ival(t, tree_dval(t) * base);
         else
            tree_set_ival(t, tree_ival(t) * base);

         tree_set_ref(t, NULL);
         tree_set_ident(t, tree_ident(decl));
      }
      return t;

   default:
      return t;
   }
}

static tree_t simp_range(tree_t t)
{
   if (tree_subkind(t) != RANGE_EXPR)
      return t;

   tree_t value = tree_value(t);
   assert(tree_kind(value) == T_ATTR_REF);

   const attr_kind_t attr = tree_subkind(value);
   assert(attr == ATTR_RANGE || attr == ATTR_REVERSE_RANGE);

   tree_t name = tree_name(value);

   type_t type = tree_type(name);
   if (type_is_unconstrained(type))
      return t;

   int dim = 0;
   if (tree_params(value) > 0) {
      int64_t ival;
      if (!folded_int(tree_value(tree_param(value, 0)), &ival))
         return t;
      dim = ival - 1;
   }

   if (attr == ATTR_REVERSE_RANGE) {
      tree_t base_r = range_of(type, dim);
      const range_kind_t base_kind = tree_subkind(base_r);
      assert(base_kind == RANGE_TO || base_kind == RANGE_DOWNTO);

      tree_t rev = tree_new(T_RANGE);
      tree_set_subkind(rev, base_kind ^ 1);
      tree_set_loc(rev, tree_loc(t));
      tree_set_type(rev, tree_type(t));
      tree_set_left(rev, tree_right(base_r));
      tree_set_right(rev, tree_left(base_r));

      return rev;
   }
   else
      return range_of(type, dim);
}

static tree_t simp_subprogram_decl(tree_t decl, simp_ctx_t *ctx)
{
   // Remove predefined operators which are hidden by explicitly defined
   // operators in the same region

   const tree_flags_t flags = tree_flags(decl);
   if ((flags & TREE_F_PREDEFINED) && (flags & TREE_F_HIDDEN))
      return NULL;

   if (ctx->subprograms != NULL && tree_subkind(decl) != S_USER)
      hash_put(ctx->subprograms, tree_ident2(decl), decl);

   return decl;
}

static tree_t simp_subprogram_body(tree_t body, simp_ctx_t *ctx)
{
   if (ctx->subprograms != NULL)
      hash_put(ctx->subprograms, tree_ident2(body), body);

   return body;
}

static tree_t simp_generic_map(tree_t t, tree_t unit)
{
   switch (tree_kind(unit)) {
   case T_CONFIGURATION:
   case T_ARCH:
      unit = tree_primary(unit);
      break;
   default:
      break;
   }

   const int ngenmaps = tree_genmaps(t);
   const int ngenerics = tree_generics(unit);

   int last_pos = 0;
   for (; last_pos < ngenmaps; last_pos++) {
      if (tree_subkind(tree_genmap(t, last_pos)) != P_POS)
         break;
   }

   if (last_pos == ngenmaps && ngenmaps == ngenerics)
      return t;

   const tree_kind_t kind = tree_kind(t);
   tree_t new = tree_new(kind);
   tree_set_loc(new, tree_loc(t));
   tree_set_ident(new, tree_ident(t));

   for (int i = 0; i < last_pos; i++)
      tree_add_genmap(new, tree_genmap(t, i));

   const int nparams = tree_params(t);
   for (int i = 0; i < nparams; i++)
      tree_add_param(new, tree_param(t, i));

   switch (kind) {
   case T_INSTANCE:
      if (tree_has_spec(t))
         tree_set_spec(new, tree_spec(t));
      // Fall-through
   case T_BINDING:
      tree_set_ref(new, tree_ref(t));
      tree_set_class(new, tree_class(t));
      if (tree_has_ident2(t))
         tree_set_ident2(new, tree_ident2(t));
      break;

   case T_BLOCK:
      {
         const int nports = tree_ports(t);
         for (int j = 0; j < nports; j++)
            tree_add_port(new, tree_port(t, j));

         for (int j = 0; j < ngenerics; j++)
            tree_add_generic(new, tree_generic(t, j));

         const int ndecls = tree_decls(t);
         for (int j = 0; j < ndecls; j++)
            tree_add_decl(new, tree_decl(t, j));

         const int nstmts = tree_stmts(t);
         for (int j = 0; j < nstmts; j++)
            tree_add_stmt(new, tree_stmt(t, j));
      }
      break;

   default:
      fatal_trace("cannot clone tree kind %s in simp_generic_map",
                  tree_kind_str(kind));
   }

   for (int i = last_pos; i < ngenerics; i++) {
      tree_t g = tree_generic(unit, i), value = NULL;
      ident_t ident = tree_ident(g);

      for (int j = last_pos; j < ngenmaps; j++) {
         tree_t mj = tree_genmap(t, j);
         assert(tree_subkind(mj) == P_NAMED);

         tree_t name = tree_name(mj);
         if (tree_kind(name) != T_REF)
            fatal_at(tree_loc(name), "sorry, this form of generic map is not "
                     "yet supported");

         if (tree_ident(name) == ident) {
            assert(value == NULL);  // TODO
            value = tree_value(mj);
         }
      }

      if (value == NULL && tree_has_value(g))
         value = tree_value(g);
      else if (value == NULL && kind == T_BINDING) {
         value = tree_new(T_OPEN);
         tree_set_loc(value, tree_loc(t));
         tree_set_type(value, tree_type(g));
      }
      else if (value == NULL)
         fatal_trace("missing value for generic %s", istr(ident));

      tree_t m = tree_new(T_PARAM);
      tree_set_loc(m, tree_loc(value));
      tree_set_subkind(m, P_POS);
      tree_set_pos(m, i);
      tree_set_value(m, value);

      tree_add_genmap(new, m);
   }

   return new;
}

static tree_t simp_tree(tree_t t, void *_ctx)
{
   simp_ctx_t *ctx = _ctx;

   switch (tree_kind(t)) {
   case T_PROCESS:
      return simp_process(t);
   case T_ARRAY_REF:
      return simp_array_ref(t);
   case T_ARRAY_SLICE:
      return simp_array_slice(t);
   case T_ATTR_REF:
      return simp_attr_ref(t, ctx);
   case T_FCALL:
   case T_PROT_FCALL:
      return simp_fcall(t, ctx);
   case T_PCALL:
   case T_PROT_PCALL:
      return simp_pcall(t);
   case T_REF:
      return simp_ref(t, ctx);
   case T_IF:
      return simp_if(t);
   case T_CASE:
      return simp_case(t);
   case T_WHILE:
      return simp_while(t);
   case T_CASSIGN:
      return simp_cassign(t);
   case T_SELECT:
      return simp_select(t);
   case T_WAIT:
      return simp_wait(t);
   case T_NULL:
      return NULL;   // Delete it
   case T_CPCALL:
      return simp_cpcall(t);
   case T_CASSERT:
      return simp_cassert(t);
   case T_RECORD_REF:
      return simp_record_ref(t);
   case T_CTXREF:
      return simp_context_ref(t, ctx);
   case T_USE:
      return simp_use(t);
   case T_ASSERT:
      return simp_assert(t);
   case T_IF_GENERATE:
      return simp_if_generate(t);
   case T_SIGNAL_ASSIGN:
      return simp_signal_assign(t);
   case T_ASSOC:
      return simp_assoc(t);
   case T_TYPE_CONV:
      return simp_type_conv(t, ctx);
   case T_LITERAL:
      return simp_literal(t);
   case T_RANGE:
      return simp_range(t);
   case T_FUNC_DECL:
   case T_PROC_DECL:
      return simp_subprogram_decl(t, ctx);
   case T_FUNC_BODY:
   case T_PROC_BODY:
      return simp_subprogram_body(t, ctx);
   case T_INSTANCE:
   case T_BINDING:
      return simp_generic_map(t, tree_ref(t));
   case T_BLOCK:
      return simp_generic_map(t, t);
   default:
      return t;
   }
}

static void simp_generics(tree_t t, simp_ctx_t *ctx)
{
   const int ngenerics = tree_generics(t);
   const int ngenmaps = tree_genmaps(t);

   for (int i = 0; i < ngenerics; i++) {
      tree_t g = tree_generic(t, i);
      unsigned pos = i;
      tree_t map = NULL;

      if (pos < ngenmaps) {
         tree_t m = tree_genmap(t, pos);
         if (tree_subkind(m) == P_POS)
            map = tree_value(m);
      }

      if (map == NULL) {
         for (int j = 0; j < ngenmaps; j++) {
            tree_t m = tree_genmap(t, j);
            if (tree_subkind(m) == P_NAMED) {
               tree_t name = tree_name(m);
               assert(tree_kind(name) == T_REF);

               if (tree_ident(name) == tree_ident(g)) {
                  map = tree_value(m);
                  break;
               }
            }
         }
      }

      if (map == NULL && tree_has_value(g))
         map = tree_value(g);

      if (map == NULL)
         continue;

      if (ctx->generics == NULL)
         ctx->generics = hash_new(128, true);

      hash_put(ctx->generics, g, map);
   }
}

static void simp_pre_cb(tree_t t, void *__ctx)
{
   simp_ctx_t *ctx = __ctx;

   switch (tree_kind(t)) {
   case T_BLOCK:
      if (tree_genmaps(t) > 0)
         simp_generics(t, ctx);
      break;
   default:
      break;
   }
}

void simplify_local(tree_t top)
{
   simp_ctx_t ctx = {
      .imp_signals = NULL,
      .top         = top,
      .exec        = exec_new(0),
      .eval_mask   = TREE_F_LOCALLY_STATIC,
   };

   tree_rewrite(top, simp_pre_cb, simp_tree, &ctx);

   exec_free(ctx.exec);

   if (ctx.generics)
      hash_free(ctx.generics);

   while (ctx.imp_signals != NULL) {
      tree_add_decl(top, ctx.imp_signals->signal);
      tree_add_stmt(top, ctx.imp_signals->process);

      imp_signal_t *tmp = ctx.imp_signals->next;
      free(ctx.imp_signals);
      ctx.imp_signals = tmp;
   }
}

void simplify_global(tree_t top, hash_t *generics)
{
   simp_ctx_t ctx = {
      .imp_signals = NULL,
      .top         = top,
      .exec        = exec_new(EVAL_FCALL),
      .eval_mask   = TREE_F_GLOBALLY_STATIC | TREE_F_LOCALLY_STATIC,
      .generics    = generics,
      .subprograms = hash_new(256, true)
   };

   exec_set_lower_fn(ctx.exec, simp_lower_cb, &ctx);

   tree_rewrite(top, simp_pre_cb, simp_tree, &ctx);

   exec_free(ctx.exec);

   if (generics == NULL && ctx.generics != NULL)
      hash_free(ctx.generics);

   hash_free(ctx.subprograms);

   assert(ctx.imp_signals == NULL);
}

//
//  Copyright (C) 2021  Nick Gasson
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

#ifndef _EXEC_H
#define _EXEC_H

#include "prim.h"
#include "phase.h"

typedef struct _eval_frame eval_frame_t;

typedef union {
   int64_t integer;
   double  real;
} eval_scalar_t;

exec_t *exec_new(eval_flags_t flags);
void exec_free(exec_t *ex);
eval_frame_t *exec_link(exec_t *ex, ident_t ident);
eval_scalar_t exec_call(exec_t *ex, ident_t func, eval_frame_t *context,
                        const char *fmt, ...);
tree_t exec_fold(exec_t *ex, tree_t expr, vcode_unit_t thunk);
eval_scalar_t exec_get_var(exec_t *ex, eval_frame_t *frame, unsigned nth);
eval_flags_t exec_get_flags(exec_t *ex);

#endif  // _EXEC_H
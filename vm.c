/**
 * Copyright 2010 Brian Taylor
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <stdlib.h>

#include "vm.h"
#include "types.h"
#include "symbols.h"
#include "interp.h"
#include "read.h"
#include "gc.h"

object *cc_bytecode;

#define OPCODE(x) CAR(x)
#define ARGS(x) CDR(x)
#define ARG1(x) CAR(ARGS(x))
#define ARG2(x) CAR(CDR(ARGS(x)))

#define length1(x) (cdr(x) == the_empty_list)

#define opcode_table(define)			\
  define(args)					\
  define(argsdot)				\
  define(return)				\
  define(const)					\
  define(fn)					\
  define(fjump)					\
  define(tjump)					\
  define(jump)					\
  define(fcallj)				\
  define(callj)					\
  define(lvar)					\
  define(save)					\
  define(gvar)					\
  define(lset)					\
  define(gset)					\
  define(setcc)					\
  define(cc)					\
  define(pop)

/* generate the symbol variable declarations */
#define generate_decls(opcode) object * opcode ## _op;
opcode_table(generate_decls)

/* generate an enumeration of all of the opcodes */
#define generate_enum(opcode) _ ## opcode ## _,
     enum {
       opcode_table(generate_enum)
	 INVALID_BYTECODE,
     } opcodes;

/* generate the stringified form */
#define generate_string(opcode) "" # opcode,
     const char *bytecode_str[] = {
       opcode_table(generate_string)
     };

object *error_sym;

object *bytecodes[INVALID_BYTECODE];

/* generate a function that converts a symbol into the corresponding
   bytecode */
#define generate_sym_to_code(opcode)		\
  if(sym == opcode ## _op) {			\
    return bytecodes[ _ ## opcode ## _ ];	\
  }

object *symbol_to_code(object *sym) {
  opcode_table(generate_sym_to_code);
  return false;
}

DEFUN1(symbol_to_code_proc) {
  return symbol_to_code(FIRST);
}

/* generate a function that converts a bytecode back into its
   corresponding symbol */

#define generate_code_to_sym(opcode)			\
  if(CHAR(FIRST) == _ ## opcode ## _) {			\
    return make_symbol("" # opcode);			\
  }

DEFUN1(code_to_symbol_proc) {
  opcode_table(generate_code_to_sym)
    return false;
}

#define VM_ASSERT(test, msg, ...)		\
  do {						\
    if(!(test)) {				\
      fprintf(stderr, msg, ##__VA_ARGS__);	\
      VM_RETURN(error_sym);			\
    }						\
  } while(0)

void vector_push(object * stack, object * thing, long top) {
  if(top == VSIZE(stack)) {
    long old_size = VSIZE(stack);
    VSIZE(stack) = old_size * 1.8;
    VARRAY(stack) = realloc(VARRAY(stack), sizeof(object *)
			    * VSIZE(stack));
    int ii;
    for(ii = old_size; ii < VSIZE(stack); ++ii) {
      VARRAY(stack)[ii] = the_empty_list;
    }
  }
  VARRAY(stack)[top++] = thing;
}

object *vector_pop(object * stack, long top) {
  object *old = VARRAY(stack)[--top];
  VARRAY(stack)[top] = the_empty_list;
  return old;
}

#define VM_RETURN(obj)				\
  do {						\
    pop_root(&top);				\
    pop_root(&env);				\
    pop_root(&fn);				\
    return obj;					\
  } while(0)

/** this code needs to be spliced into several
 *  locations so we just define it once here
 */
#define RETURN_OPCODE_INSTRUCTIONS			\
  /* if there's only one value on the stack,		\
   * we're done */                                      \
  if(stack_top == initial_top + 1) {			\
    object *val;					\
    VPOP(val, stack, stack_top);			\
    VM_RETURN(val);					\
  } else {						\
    object *val;					\
    VPOP(val, stack, stack_top);			\
    object *ret_addr;					\
    VPOP(ret_addr, stack, stack_top);			\
    /* retore what we stashed away in save */		\
    fn = car(cdr(ret_addr));				\
    pc = LONG(car(ret_addr));				\
    env = cdr(cdr(ret_addr));				\
    /* setup for the next loop */			\
    VPUSH(val, stack, stack_top);			\
    goto vm_fn_begin;					\
  }

#ifdef VM_DEBUGGING
#define VM_DEBUG(msg, obj)			\
  do {						\
    fprintf(stdout, "%s: ", msg);		\
    owrite(stdout, obj);			\
    fprintf(stdout, "\n");			\
  } while(0)
#else
#define VM_DEBUG(msg, obj)
#endif

object *vm_execute(object * fn, object * stack, long stack_top, long n_args) {
  object *code_array;
  object *env;
  object *instr;
  object *opcode;
  object *top;

  long initial_top = stack_top - n_args;
  long pc = 0;

  /* bootstrap an empty frame for this function since the callj opcode
     won't have built one for us */

  env = CENV(fn);
  env = cons(the_empty_vector, env);

  instr = the_empty_list;
  top = the_empty_list;

  push_root(&fn);
  push_root(&env);
  push_root(&top);

vm_fn_begin:
  VM_ASSERT(is_compiled_proc(fn) || is_compiled_syntax_proc(fn),
	    "object is not compiled-procedure");

  code_array = BYTECODE(fn);
  VM_DEBUG("bytecode", code_array);
  VM_DEBUG("stack", stack);

  object **codes = VARRAY(code_array);
  long num_codes = VSIZE(code_array);

vm_begin:
  if(pc >= num_codes) {
    VM_ASSERT(false, "pc flew off the end of memory");
  }

  instr = codes[pc++];
  opcode = OPCODE(instr);

  VM_DEBUG("dispatching", instr);

  switch (CHAR(opcode)) {
  case _args_:{
      if(n_args != LONG(ARG1(instr))) {
	VM_ASSERT(0, "wrong number of args. expected %ld, got %ld\n",
		  LONG(ARG1(instr)), n_args);
      }

      int ii;
      int num_args = LONG(ARG1(instr));

      /* resize the top frame if we need to */
      if(num_args > VSIZE(car(env))) {
	set_car(env, make_vector(the_empty_list, num_args));
      }

      object *vector = car(env);
      object **vdata = VARRAY(vector);
      for(ii = num_args - 1; ii >= 0; --ii) {
	VPOP(top, stack, stack_top);
	vdata[ii] = top;
      }

      VM_DEBUG("after_args environment", env);
    }
    break;
  case _argsdot_:{
      VM_ASSERT(n_args >= LONG(ARG1(instr)), "wrong number of args");

      int ii;
      long req_args = LONG(ARG1(instr));
      long array_size = req_args + 1;

      /* resize the top frame if we need to */
      if(array_size > VSIZE(car(env))) {
	set_car(env, make_vector(the_empty_list, array_size));
      }

      object *vector = car(env);
      object **vdata = VARRAY(vector);

      /* push the excess args onto the last position, top is
         protected */
      vdata[array_size - 1] = the_empty_list;
      for(ii = 0; ii < n_args - req_args; ++ii) {
	VPOP(top, stack, stack_top);
	vdata[array_size - 1] = cons(top, vdata[array_size - 1]);
      }

      /* now pop off the required args into their positions */
      for(ii = req_args - 1; ii >= 0; --ii) {
	VPOP(top, stack, stack_top);
	vdata[ii] = top;
      }

      VM_DEBUG("after_args environment", env);
    }
    break;
  case _fjump_:{
      VPOP(top, stack, stack_top);
      if(is_falselike(top)) {
	pc = LONG(ARG1(instr));
      }
    }
    break;
  case _tjump_:{
      VPOP(top, stack, stack_top);
      if(!is_falselike(top)) {
	pc = LONG(ARG1(instr));
      }
    }
    break;
  case _jump_:{
      pc = LONG(ARG1(instr));
    }
    break;
  case _fn_:{
      object *fn_arg = ARG1(instr);
      object *new_fn = make_compiled_proc(BYTECODE(fn_arg),
					  env);
      push_root(&new_fn);
      VPUSH(new_fn, stack, stack_top);
      pop_root(&new_fn);
    }
    break;
  case _fcallj_:{
    VPOP(top, stack, stack_top);

    /* unwrap meta */
    if(is_meta(top)) {
      top = METAPROC(top);
    }

    long args_for_call = LONG(ARG1(instr));

    if(is_compiled_proc(top) || is_compiled_syntax_proc(top)) {
      fn = top;
      pc = 0;
      n_args = args_for_call;

      /* generate a fresh frame for the callee */
      env = CENV(fn);

      object *newframe = make_vector(the_empty_list, n_args + 1);
      push_root(&newframe);
      env = cons(newframe, env);
      pop_root(&newframe);

      goto vm_fn_begin;
    }
    else if(is_primitive_proc(top)) {
      /* build the list the target expects for the call */
      long ii;
      object *pfn = top;
      push_root(&pfn);

      top = pfn->data.primitive_proc.fn(stack, args_for_call, stack_top);
      /* unwind the stack since primitives don't clean up after
	 themselves */
      object *temp;
      for(ii = 0; ii < args_for_call; ++ii) {
	VPOP(temp, stack, stack_top);
      }

      VPUSH(top, stack, stack_top);
      pop_root(&pfn);

      RETURN_OPCODE_INSTRUCTIONS;
    }
    else {
      owrite(stderr, top);
      fprintf(stderr, "\n");
      VM_ASSERT(0, "don't know how to invoke");
    }
  }
    break;
  case _callj_:{
      VPOP(top, stack, stack_top);

      /* unwrap meta */
      if(is_meta(top)) {
	top = METAPROC(top);
      }

      long args_for_call = LONG(ARG1(instr));

      /* special case for apply (which will always be callj) */
      if(args_for_call == -1) {
	/* function is on the top of the stack (as usual) */
	object *target_fn = top;
	push_root(&target_fn);

	/* and the args are in a list next, expand those */
	VPOP(top, stack, stack_top);

	args_for_call = 0;
	while(!is_the_empty_list(top)) {
	  VPUSH(car(top), stack, stack_top);
	  top = cdr(top);
	  ++args_for_call;
	}

	/* and put back the fn for the dispatch */
	top = target_fn;
	pop_root(&target_fn);
      }

      if(is_compiled_proc(top) || is_compiled_syntax_proc(top)) {
	fn = top;
	pc = 0;
	n_args = args_for_call;

	/* we can reuse the cons and frame from our environment to
	   build the new one */
	object *fn_env = CENV(fn);
	set_cdr(env, fn_env);

	goto vm_fn_begin;
      }
      else if(is_primitive_proc(top)) {
	/* build the list the target expects for the call */
	long ii;
	object *pfn = top;
	push_root(&pfn);

	top = pfn->data.primitive_proc.fn(stack, args_for_call, stack_top);
	/* unwind the stack since primitives don't clean up after
	   themselves */
	object *temp;
	for(ii = 0; ii < args_for_call; ++ii) {
	  VPOP(temp, stack, stack_top);
	}

	VPUSH(top, stack, stack_top);
	pop_root(&pfn);

	RETURN_OPCODE_INSTRUCTIONS;
      }
      else {
	owrite(stderr, top);
	fprintf(stderr, "\n");
	VM_ASSERT(0, "don't know how to invoke");
      }
    }
    break;
  case _lvar_:{
      int env_num = LONG(ARG1(instr));
      int idx = LONG(ARG2(instr));

      object *next = env;
      while(env_num-- > 0) {
	next = cdr(next);
      }

      object *data = VARRAY(car(next))[idx];
      VPUSH(data, stack, stack_top);
    }
    break;
  case _lset_:{
      int env_num = LONG(ARG1(instr));
      int idx = LONG(ARG2(instr));

      object *next = env;
      while(env_num-- > 0) {
	next = cdr(next);
      }

      VARRAY(car(next))[idx] = VARRAY(stack)[stack_top - 1];
    }
    break;
  case _gvar_:{
      object *var = lookup_global_value(ARG1(instr), vm_global_environment);
      push_root(&var);
      VPUSH(var, stack, stack_top);
      pop_root(&var);
    }
    break;
  case _gset_:{
      object *var = ARG1(instr);
      object *val = VARRAY(stack)[stack_top - 1];
      define_global_variable(var, val, vm_global_environment);
    }
    break;
  case _setcc_: {
    object *new_stack;
    object *new_stack_top;

    VPOP(new_stack, stack, stack_top);
    VPOP(new_stack_top, stack, stack_top);

    stack = car(stack);
    stack_top = LONG(new_stack_top);
  }
    break;
  case _cc_: {
    object *cc_env = make_vector(the_empty_list, 2);
    push_root(&cc_env);

    /* copy the stack */
    object *new_stack = make_vector(the_empty_list, VSIZE(stack));
    push_root(&new_stack);
    long idx;
    for(idx = 0; idx < stack_top; ++idx) {
      VARRAY(new_stack)[idx] = VARRAY(stack)[idx];
    }

    /* insert it into the environment */
    VARRAY(cc_env)[0] = new_stack;
    VARRAY(cc_env)[1] = make_fixnum(stack_top);
    pop_root(&new_stack);

    cc_env = cons(cc_env, the_empty_list);

    object *cc_fn = make_compiled_proc(cc_bytecode, cc_env);
    pop_root(&cc_env);

    VPUSH(cc_fn, stack, stack_top);
  }
    break;
  case _pop_:{
      VPOP(top, stack, stack_top);
    }
    break;
  case _save_:{
      object *ret_addr = cons(fn, env);
      push_root(&ret_addr);
      ret_addr = cons(ARG1(instr), ret_addr);
      VPUSH(ret_addr, stack, stack_top);
      pop_root(&ret_addr);
    }
    break;
  case _return_:{
      RETURN_OPCODE_INSTRUCTIONS;
    }
    break;
  case _const_:{
      VPUSH(ARG1(instr), stack, stack_top);
    }
    break;
  default:{
      fprintf(stderr, "don't know how to process ");
      owrite(stderr, opcode);
      fprintf(stderr, "\n");
      VM_ASSERT(0, "strange opcode");
    }
  }

  goto vm_begin;
  VM_RETURN(error_sym);
}

DEFUN1(vm_tag_macro_proc) {
  FIRST->type = COMPILED_SYNTAX_PROC;
  return FIRST;
}

#define generate_syminit(opcode) opcode ## _op = make_symbol("" # opcode);
#define generate_bytecodes(opcode)				  \
  bytecodes[_ ## opcode ## _] = make_character(_ ## opcode ## _); \
  push_root(&bytecodes[_ ## opcode ## _]);


object *make_instr(char *name, object *arg1, object *arg2) {
  object *sym = symbol_to_code(make_symbol(name));
  object *result = the_empty_list;
  push_root(&result);
  result = cons(arg2, result);
  result = cons(arg1, result);
  result = cons(sym, result);
  pop_root(&result);
  return result;
}

void vm_init(void) {
  /* generate the symbol initializations */
  opcode_table(generate_syminit)

    opcode_table(generate_bytecodes)

    error_sym = make_symbol("error");

  object *curr = make_primitive_proc(vm_tag_macro_proc);

  push_root(&curr);
  define_global_variable(make_symbol("set-macro!"),
			 curr, vm_global_environment);
  pop_root(&curr);

  /* the cc opcode needs a little special bytecode to do its thing */
  cc_bytecode = make_vector(the_empty_list, 6);
  push_root(&cc_bytecode);

  object **codes = VARRAY(cc_bytecode);
  codes[0] = make_instr("args", make_fixnum(1), the_empty_list);
  codes[1] = make_instr("lvar", make_fixnum(1), make_fixnum(1)); /* top */
  codes[2] = make_instr("lvar", make_fixnum(1), make_fixnum(0)); /* stack */
  codes[3] = make_instr("setcc", the_empty_list, the_empty_list);
  codes[4] = make_instr("lvar", make_fixnum(0), make_fixnum(0)); /* fn */
  codes[5] = make_instr("return", the_empty_list, the_empty_list);
}

void vm_init_environment(object * env) {
  object *curr = the_empty_list;
  push_root(&curr);

  define_global_variable(make_symbol("symbol->bytecode"),
			 curr = make_primitive_proc(symbol_to_code_proc),
			 env);

  define_global_variable(make_symbol("bytecode->symbol"),
			 curr = make_primitive_proc(code_to_symbol_proc),
			 env);

  pop_root(&curr);
}

void wb(object * vector) {
  long idx = 0;
  long size = VSIZE(vector);
  object **codes = VARRAY(vector);

  fprintf(stderr, "#<bytecode: ");
  for(idx = 0; idx < size; ++idx) {
    int code = (int)CHAR(OPCODE(codes[idx]));
    fprintf(stderr, "(%s . ", bytecode_str[code]);
    owrite(stderr, cdr(codes[idx]));
    fprintf(stderr, ") ");
  }
  fprintf(stderr, ">\n");
}

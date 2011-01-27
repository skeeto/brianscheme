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

#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#include "types.h"
#include "symbols.h"
#include "hashtab.h"
#include "pool.h"

/* enable gc debuging by defining
 * DEBUG_GC
 */

pool_t *global_pool;

void *MALLOC(size_t size) {
  void *obj = pool_alloc(global_pool, size);
  if(obj == NULL) {
    fprintf(stderr, "out of memory\n");
    exit(1);
  }
  return obj;
}

void *REALLOC(void *p, size_t old, size_t new) {
  void *np = MALLOC(new);
  memcpy(np, p, old);
  return np;
}

void FREE(void *p) {
  (void) p;
  /* You're free!!! */
}

void *xmalloc(size_t size) {
  void *obj = malloc(size);
  if(obj == NULL) {
    fprintf(stderr, "out of memory\n");
    exit(1);
  }
  return obj;
}

void throw_gc_va(char *msg, va_list args) {
  vfprintf(stderr, msg, args);
  exit(2);
}

void throw_gc(char *msg, ...) {
  va_list args;
  va_start(args, msg);
  throw_gc_va(msg, args);
  va_end(args);
}

#ifdef DEBUG_GC
#include <execinfo.h>

void print_backtrace() {
#define MAX_FRAMES 30
  void *buffer[MAX_FRAMES];
  int frames = backtrace(buffer, MAX_FRAMES);
  backtrace_symbols_fd(buffer, frames, 2);
}

void debug_gc(char *msg, ...) {
  va_list args;
  va_start(args, msg);
  vfprintf(stderr, msg, args);
  va_end(args);
}

void assert_gc(char test, char *msg, ...) {
  va_list args;
  va_start(args, msg);

  if(!test) {
    throw_gc_va(msg, args);
  }

  va_end(args);
}
#else
#define assert_gc(a, b, ...)
#define print_backtrace()
#define debug_gc(msg, ...)
#endif


typedef struct doubly_linked_list {
  object *head;
  object *tail;
  long num_objects;
} doubly_linked_list;

void move_object_to_head(object * obj, doubly_linked_list * src,
			 doubly_linked_list * dest) {
  /* unlink from the old list */
  if(obj->prev == NULL) {
    src->head = obj->next;
  }
  else {
    obj->prev->next = obj->next;
  }
  if(obj->next == NULL) {
    src->tail = obj->prev;
  }
  else {
    obj->next->prev = obj->prev;
  }
  src->num_objects--;

  /* link into new list */
  if(dest->head == NULL) {
    dest->head = obj;
    dest->tail = obj;
    obj->next = NULL;
    obj->prev = NULL;
  }
  else {
    obj->prev = NULL;
    obj->next = dest->head;
    obj->next->prev = obj;
    dest->head = obj;
  }

  dest->num_objects++;
}

void append_to_tail(doubly_linked_list * dest, doubly_linked_list * src) {
  if(dest->tail == NULL) {
    dest->head = src->head;
    dest->tail = src->tail;
  }
  else if(src->head == NULL) {
    return;
  }
  else {
    /* link end of dest to start of src */
    dest->tail->next = src->head;
    dest->tail->next->prev = dest->tail;
    dest->tail = src->tail;
  }

  dest->num_objects += src->num_objects;

  src->head = NULL;
  src->tail = NULL;
  src->num_objects = 0;
}

/* these debug_* functions are far too slow to be called at normal
 * runtime but they're really useful for calling from gdb to make sure
 * my assumptions are holding at each step of garbage collection.
 */
#ifdef DEBUG_GC
long debug_list_contains(doubly_linked_list * list, object * obj) {
  object *iter = list->head;
  if(iter == list->tail) {
    assert_gc(iter == obj, "object %p not in length1 list\n", obj);
    return 0;
  }

  long pos = 0;
  while(iter != list->tail) {
    if(iter == obj) {
      return pos;
    }
    iter = iter->next;
    ++pos;
  }
  assert_gc(iter == obj, "object %p not in list\n", obj);
  return list->num_objects - 1;
}


void debug_validate(doubly_linked_list * list) {
  /* verify the structure of a linked list */
  if(list->head == NULL || list->tail == NULL) {
    assert_gc(list->head == NULL &&
	      list->tail == NULL, "head and tail must be null together");
    assert_gc(list->num_objects == 0, "head is null. count != 0");
    return;
  }

  assert_gc(list->head->prev == NULL, "head's prev is not null");

  if(list->head == list->tail) {
    assert_gc(list->num_objects == 1, "1 length list invalid");
    assert_gc(list->head->next == NULL, "next of only item not null");
    assert_gc(list->tail->prev == NULL, "prev of only item not null");
    return;
  }

  assert_gc(list->head->next != NULL, "list head next is null");

  object *iter = list->head->next;
  object *last = NULL;

  long idx = 1;
  while(iter != list->tail) {
    assert_gc(iter->prev != NULL, "central node %ld prev is null", idx);
    assert_gc(iter->next != NULL, "central node %ld next is null", idx);
    if(last) {
      assert_gc(iter->prev == last,
		"central node %ld prev is wrong. Is %p. Should be %p",
		idx, iter->prev, last);
    }

    ++idx;

    last = iter;
    iter = iter->next;
  }

  ++idx;
  assert_gc(iter->next == NULL, "list tail next is not null");
  assert_gc(iter->prev != NULL, "list tail prev is null");
  assert_gc(iter->prev == last,
	    "list tail prev is wrong. Is %p. Should be %p", iter->prev, last);
  assert_gc(idx == list->num_objects,
	    "list object count is wrong %ld != %ld", idx, list->num_objects);
}
#else
#define debug_list_contains(a, b)
#define debug_validate(a)
#endif

typedef struct stack_set {
  void **objs;
  long top;
  long size;
} stack_set;

stack_set *make_stack_set(int initial_size) {
  stack_set *ss = MALLOC(sizeof(stack_set));
  ss->top = 0;
  ss->size = initial_size;
  ss->objs = MALLOC(sizeof(void *) * initial_size);
  return ss;
}

void clear_stack_set(stack_set * ss) {
  ss->top = 0;
}

void stack_set_push(stack_set * ss, void *value) {
  /* grow the stack if we need to */
  if(ss->top == ss->size) {
    long old_size = ss->size;
    long new_size = ss->size * 2;
    ss->objs = REALLOC(ss->objs, sizeof(void *) * old_size,
		       sizeof(void *) * new_size);
    ss->size = new_size;
  }

  ss->objs[ss->top++] = value;
}

char stack_set_pop(stack_set * ss, void *value) {
  if(ss->objs[--ss->top] != value) {
    /* scan back until we find it */
    int idx = ss->top - 1;
    object **last = ss->objs[ss->top];
    int done = 0;
    for(; idx >= 0 && !done; --idx) {
      if(ss->objs[idx] == value) {
	done = 1;
      }
      object **temp = ss->objs[idx];
      ss->objs[idx] = last;
      last = temp;
    }
    return done;
  }
  return 1;
}

static doubly_linked_list Active_Heap_Objects;
static doubly_linked_list Old_Heap_Objects;

static object *Next_Free_Object = NULL;
struct stack_set *Root_Objects = NULL;
struct stack_set *Finalizable_Objects = NULL;
struct stack_set *Finalizable_Objects_Next = NULL;

char current_color = 0;

void extend_heap(long);

void gc_init(void) {
  global_pool = create_pool(0);

  Root_Objects = make_stack_set(400);
  Finalizable_Objects = make_stack_set(400);
  Finalizable_Objects_Next = make_stack_set(400);

  Active_Heap_Objects.head = NULL;
  Active_Heap_Objects.tail = NULL;
  Active_Heap_Objects.num_objects = 0;

  Old_Heap_Objects.head = NULL;
  Old_Heap_Objects.tail = NULL;
  Old_Heap_Objects.num_objects = 0;

  extend_heap(1000);

  /* everything is free right now */
  Next_Free_Object = Active_Heap_Objects.head;
}

object *push_root(object ** root) {
  stack_set_push(Root_Objects, root);
  return *root;
}

void pop_root(object ** root) {
  if(!stack_set_pop(Root_Objects, root)) {
    print_backtrace();
    throw_gc("pop_stack_root - object not found\n");
  }
}

/* extends the front of the heap. assumes the heap has already
 * been scavanged for any live objects
 */
void extend_heap(long extension) {
  int ii;
  object *new_heap = MALLOC(sizeof(object) * extension);

  new_heap[0].prev = NULL;
  new_heap[0].next = &new_heap[1];
  new_heap[0].color = current_color;

  for(ii = 1; ii < extension - 1; ++ii) {
    new_heap[ii].next = &new_heap[ii + 1];
    new_heap[ii].prev = &new_heap[ii - 1];
    new_heap[ii].color = current_color;
  }

  const long last = extension - 1;
  new_heap[last].next = Active_Heap_Objects.head;
  new_heap[last].prev = &new_heap[last - 1];

  if(Active_Heap_Objects.head) {
    Active_Heap_Objects.head->prev = &new_heap[last];
  }
  else {
    /* this is the first heap allocation */
    Active_Heap_Objects.tail = &new_heap[last];
  }
  new_heap[last].color = current_color;

  Active_Heap_Objects.head = new_heap;

  /* bump next free back */
  Next_Free_Object = new_heap;

  Active_Heap_Objects.num_objects += extension;
  debug_validate(&Active_Heap_Objects);
}

void move_reachable(object * root, doubly_linked_list * to_set) {
  int ii;
  hashtab_iter_t htab_iter;

  if(root == NULL)
    return;
  if(root->color == current_color)
    return;

  /* mark this and move it into the to_set we will be building a queue
     of objects to scan from the front and scanning in the prev
     direction */
  root->color = current_color;
  move_object_to_head(root, &Active_Heap_Objects, to_set);

  object *scan_iter = to_set->head;

  /* we do the same thing a lot... make a macro! */
  object *temp;
#define maybe_move(obj)						\
  do {								\
    temp = obj;							\
    if(temp->color != current_color) {				\
      move_object_to_head(temp, &Active_Heap_Objects, to_set);	\
      temp->color = current_color;				\
    }								\
  } while(0)

  while(scan_iter != NULL) {
    /* scan fields */
    switch (scan_iter->type) {
    case PAIR:
      maybe_move(CAR(scan_iter));
      maybe_move(CDR(scan_iter));
      break;
    case COMPOUND_PROC:
    case SYNTAX_PROC:
      maybe_move(COMPOUND_ENV(scan_iter));
      maybe_move(COMPOUND_PARAMS(scan_iter));
      maybe_move(COMPOUND_BODY(scan_iter));
      break;
    case VECTOR:
      for(ii = 0; ii < VSIZE(scan_iter); ++ii) {
	maybe_move(VARRAY(scan_iter)[ii]);
      }
      break;
    case COMPILED_PROC:
    case COMPILED_SYNTAX_PROC:
      maybe_move(BYTECODE(scan_iter));
      maybe_move(CENV(scan_iter));
      break;
    case META_PROC:
      maybe_move(METAPROC(scan_iter));
      maybe_move(METADATA(scan_iter));
      break;
    case HASH_TABLE:
      htb_iter_init(HTAB(scan_iter), &htab_iter);
      while(htab_iter.key != NULL) {
	maybe_move((object *) htab_iter.key);
	maybe_move((object *) htab_iter.value);
	htb_iter_inc(&htab_iter);
      }
    default:
      break;
    }
    scan_iter = scan_iter->prev;
  }
}

void finalize_object(object * head) {
  /* free any extra memory associated with this type */
  switch (head->type) {
  case STRING:
    FREE(head->data.string.value);
    break;
  case VECTOR:
    FREE(VARRAY(head));
    break;
  case HASH_TABLE:
    htb_destroy(HTAB(head));
  default:
    break;
  }
}

long baker_collect() {
  /* merge everything into one big heap */
  append_to_tail(&Active_Heap_Objects, &Old_Heap_Objects);

  /* move everything reachable from a root into the old set */
  ++current_color;
  int ii = 0;
  for(ii = 0; ii < Root_Objects->top; ++ii) {
    object **next = Root_Objects->objs[ii];
    move_reachable(*next, &Old_Heap_Objects);
  }

  /* now finalize anything that needs it */
  long idx = 0;
  for(idx = 0; idx < Finalizable_Objects->top; ++idx) {
    object *obj = Finalizable_Objects->objs[idx];
    if(obj->color != current_color) {
      finalize_object(obj);
    }
    else {
      stack_set_push(Finalizable_Objects_Next, obj);
    }
  }

  /* now swap the stacks and clear the old one */
  stack_set *temp = Finalizable_Objects;
  Finalizable_Objects = Finalizable_Objects_Next;
  Finalizable_Objects_Next = temp;
  clear_stack_set(Finalizable_Objects_Next);

  ++current_color;

  /* both sets should be valid */
  debug_validate(&Old_Heap_Objects);
  debug_validate(&Active_Heap_Objects);

  /* now everything left in Active is garbage and can be reused */
  Next_Free_Object = Active_Heap_Objects.head;
  long num_free = Active_Heap_Objects.num_objects;


  return num_free;
}

static long Alloc_Count = 0;
static long Next_Heap_Extension = 1000;

object *alloc_object(char needs_finalization) {
  /* always scavenge while we're debugging
     baker_collect();
   */

  if(Next_Free_Object == NULL) {
    debug_gc("no space. trying baker-collect\n");
    print_backtrace();

    /* comment this out to turn off gc
     */
    long freed = baker_collect();

    /* did we free enough? */
    if(freed == 0 || Next_Heap_Extension / freed > 2) {
      debug_gc("only freed %ld. extending the heap by %ld\n",
	       freed, Next_Heap_Extension);
      extend_heap(Next_Heap_Extension);
      Next_Heap_Extension *= 3;
    }

    if(Next_Free_Object == NULL) {
      throw_gc("extend_heap didn't work");
    }
  }

  object *obj = Next_Free_Object;
  obj->color = current_color;

  if(needs_finalization) {
    stack_set_push(Finalizable_Objects, obj);
  }

  Next_Free_Object = Next_Free_Object->next;

  return obj;
}

long get_alloc_count() {
  return Alloc_Count;
}

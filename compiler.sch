; Copyright 2010 Brian Taylor
;
; Licensed under the Apache License, Version 2.0 (the "License");
; you may not use this file except in compliance with the License.
; You may obtain a copy of the License at
;
; http://www.apache.org/licenses/LICENSE-2.0
;
; Unless required by applicable law or agreed to in writing, software
; distributed under the License is distributed on an "AS IS" BASIS,
; WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
; See the License for the specific language governing permissions and
; limitations under the License.
;

; DESCRIPTION:
;
; This is essentially a direct translation of the bytecode compiler
; presented in Peter Norvig's "Paradigms of Artificial Intelligence
; Programming." The bytecode should be identitcal to the bytecode
; produced in the book.
;
; The 'bytecodes' generated by this compiler can be executed on the
; virtual machine defined in vm.c. These bytecodes are also suitable
; for further translation and optimization. The eventual goal is
; to turn them into native assembly so user defined routines can
; execute with the same performance as interpreter primitives.


;; Comment out the second form for loads of function trace
;; information. I should really write a real trace macro at some
;; point.

(define (write-dbg1 . args)
  "display the given forms"
  (display "debug: ")
  (display args)
  (newline)
  args)

(define (write-dbg . args)
  "do nothing."
  #t)

;(define write-dbg write-dbg1)

(define (write-passthrough arg)
  (apply write-dbg arg)
  arg)

(define (write-passthrough arg)
  arg)

(define (comp-bound? sym)
  "is the symbol defined in the compiled global environment?"
  (let* ((sentinal (gensym))
	 (result (hashtab-ref *vm-global-environment* sym sentinal)))
    (not (eq? result sentinal))))

(define (comp-global-ref sym)
  "return the global from the compiled env. error if not defined."
  (let* ((sentinal (gensym))
	 (result (cdr (hashtab-ref *vm-global-environment* sym sentinal))))
    (if (eq? result sentinal)
	(throw-error "symbol" sym "is not defined in compiled env")
	result)))

(define (comp-macro? sym)
  "is a given symbol a macro in the compiled environment?"
  (and (comp-bound? sym)
       (compiled-syntax-procedure? (comp-global-ref sym))))

(define (comp-macroexpand0 form)
  "expand form using a macro found in the compiled environment"
  (apply (comp-global-ref (car form)) (cdr form)))

(define (comp x env val? more?)
  "compile an expression in the given environment optionally caring
about its value and optionally with more forms following"
  (write-dbg 'comp x 'val? val? 'more? more?)
  (cond
   ((variable-reference? x) (comp-var x env val? more?))
   ((atom? x) (comp-const x val? more?))
   (else
    (record-case x
      (if-compiling (then else)
        (comp then env val? more?))
      (quote (obj)
        (comp-const obj val? more?))
      (begin exps
        (comp-begin exps env val? more?))
      (set! (sym val)
        (seq (comp val env #t #t)
	     (gen-set sym env)
	     (when (not val?) (gen 'pop))
	     (unless more? (seq (gen 'endframe 1)
				(gen 'return)))))
      (if (test then . else)
	  (let ((else (car-else else nil)))
	    (comp-if test then else env val? more?)))
      (lambda (args . body)
	(when val?
	  (let ((f (comp-lambda args body env)))
	    (seq (gen 'fn f)
		 (unless more? (seq (gen 'endframe 1)
				    (gen 'return)))))))

      ;; generate an invocation
      (else
       (if (comp-macro? (first x))
	   (comp (comp-macroexpand0 x) env val? more?)
	   (comp-funcall (first x) (rest x)
			 env val? more?)))))))

(define (%<=2 a b)
  (or (%fixnum-less-than a b) (%fixnum-equal a b)))

(define (%<= . values)
  (every-pair? %<=2 values))

(define (%arg-count form min max)
  (let ((n-args (length (rest form))))
    (unless (%<= min n-args max)
	    (throw-error "wrong number of args"
			 form
			 "expected between"
			 min "and" max))))

(define (comp-begin exps env val? more?)
  (write-dbg 'comp-begin exps 'val? val? 'more? more?)
  (cond ((null? exps) (comp-const nil val? more?))
	((length=1 exps) (comp (first exps) env val? more?))
	(else (seq (comp (first exps) env #f #t)
		   (comp-begin (rest exps) env val? more?)))))

(define (comp-list exps env)
  (write-dbg 'comp-list exps)
  (if (null? exps) nil
      (seq (comp (first exps) env #t #t)
	   (comp-list (rest exps) env))))

(define (comp-const x val? more?)
  (write-dbg 'comp-const x 'val? val? 'more? more?)
  (when val? (seq (gen 'const x)
		  (unless more? (seq (gen 'endframe 1)
				     (gen 'return))))))

(define (comp-var x env val? more?)
  (write-dbg 'comp-var x 'val? val? 'more? more?)
  (when val? (seq (gen-var x env)
		  (unless more? (seq (gen 'endframe 1)
				     (gen 'return))))))

(define (false? exp)
  (or (null? exp) (eq? exp #f) (eq? exp 'nil)))

(define (true? exp)
  (eq? exp #t))

(define (comp-if pred then else env val? more?)
  (write-dbg 'comp-if pred 'then then 'else else
	     'val? val? 'more? more?)
  (cond
   ((false? pred)
    (comp else env val? more?))
   ((true? pred)
    (comp then env val? more?))
   (else (let ((pcode (comp pred env #t #t))
	       (tcode (comp then env val? more?))
	       (ecode (comp else env val? more?)))
	   (cond
	    ((equal? tcode ecode)
	     (seq (comp pred env #f #t)
		  ecode))
	    ((null? tcode)
	     (let ((l2 (gen-label)))
	       (seq pcode
		    (gen 'tjump l2) ecode (list l2)
		    (unless more? (seq (gen 'endframe 1)
				       (gen 'return))))))
	    ((null? ecode)
	     (let ((l1 (gen-label)))
	       (seq pcode
		    (gen 'fjump l1) tcode (list l1)
		    (unless more? (seq (gen 'endframe 1)
				       (gen 'return))))))
	    (else
	     (let ((l1 (gen-label))
		   (l2 (when more? (gen-label))))
	       (seq pcode (gen 'fjump l1) tcode
		    (when more? (gen 'jump l2))
		    (list l1) ecode
		    (when more? (list l2))))))))))

(define (comp-funcall f args env val? more?)
  (write-dbg 'comp-funcall f 'args args
	     'val? val? 'more? more?)

  (cond
   ((and (starts-with? f 'lambda eq?) (null? (second f)))
    (unless (null? args) (throw-error "too many arguments"))
    (comp-begin (cdr (cdr f)) env val? more?))
   (more?
    (let ((k (gen-label 'k)))
      (seq (gen 'save k)
	   (comp-list args env)
	   (comp f env #t #t)
	   (gen 'incprof 0)
	   (gen 'callj (length args) #t)
	   (list k)
	   (unless val? (gen 'pop)))))
   (else
    (seq (comp-list args env)
	 (comp f env #t #t)
	 (gen 'incprof 0)
	 (gen 'endframe (%fixnum-add (length args) 1))
	 (gen 'callj (length args) #f)))))

(define-struct fn
  "a structure representing a compiled function"
  (code
   env
   name
   args))

(define (comp-lambda args body env)
  "generate code for BODY with ARGS in ENV. Only generates a new
chainframe if ARGS is non-nil"
  (write-dbg 'comp-lambda args 'body body)

  ;; compute the set of free variables and the set of stack variables
  (let ((free-args (filter variable-is-free-ref (make-true-list args)))
	(stack-idx 0)
	(frame-idx 0))

    ;; assign frame positions to the free variables
    (dolist (arg free-args)
      (variable-idx-set! arg frame-idx)
      (%inc! frame-idx))

    ;; assign stack positions to each of the remaining arguments
    (dolist (arg (make-true-list args))
      (unless (variable-is-free-ref arg)
        (variable-idx-set! arg stack-idx))
      (%inc! stack-idx))

    (let ((new-env (if free-args
		       (cons free-args env)
		       env)))

      (new-fun (seq (%gen-args args)
		    (comp-begin body
				new-env
				#t #f))
	       env "unknown" args))))

(define (%count-free-args args)
  (reduce (lambda (count arg)
	    (if (variable-is-free-ref arg)
		(%fixnum-add count 1)
		count))
	  (make-true-list args)
	  0))

(define (%gen-chainframe args)
  "generate a chainframe instruction if there are any free variables"
  (let ((n-free-args (%count-free-args args)))
    (when (%fixnum-greater-than n-free-args 0)
      (gen 'chainframe n-free-args))))

(define (%gen-args args)
  (%gen-args-iter args args 0))

(define (%gen-args-iter args full-args n-so-far)
  (write-dbg '%gen-args-iter args n-so-far)
  (cond
   ((null? args)
    (%gen-chainframe full-args))

   ((variable? args)
    (seq (%gen-chainframe full-args)
	 (gen 'pushvarargs n-so-far)
	 (when (variable-is-free-ref args)
	   (gen 'lset 0 (variable-idx-ref args)))))

   ((and (pair? args)
	 (variable? (first args)))
    (let ((arg (first args)))
      (if (variable-is-free-ref arg)
	  (seq (%gen-args-iter (rest args) full-args
			       (%fixnum-add n-so-far 1))
	       (gen 'spush n-so-far)
	       (gen 'lset 0 (variable-idx-ref arg))
	       (gen 'pop))
	  (%gen-args-iter (rest args) full-args
			  (%fixnum-add n-so-far 1)))))
   (else (throw-error "illegal argument list" args))))

;; this doesn't do error checking like the method before
(define (num-args args)
  (letrec ((iter (lambda (lst count)
		   (cond
		    ((null? lst) count)
		    ((symbol? lst) (%fixnum-add count 1))
		    (else (iter (rest lst) (%fixnum-add count 1)))))))
    (iter args 0)))

(define (make-true-list dotted-list)
  (cond
   ((null? dotted-list) nil)
   ((atom? dotted-list) (list dotted-list))
   (else (cons (first dotted-list)
	       (make-true-list (rest dotted-list))))))

(define (new-fun code env name args)
  (assemble (make-fn 'code (optimize code)
		     'env env
		     'name name
		     'args args)))

(let ((label-num 0))
  (define (compiler x)
    (set! label-num 0)
    (comp-lambda nil (variable-usages (list x) nil) nil))

  (define (gen-label . opt)
    (let ((prefix (if (pair? opt)
		      (string (car opt))
		      "L")))
      (write-dbg 'gen-label prefix)
      (set! label-num (%fixnum-add label-num 1))
      (string->symbol
       (prim-concat prefix (number->string label-num))))))

(define (gen opcode . args)
  (write-dbg 'gen opcode 'args args)
  (list (cons opcode args)))

(define (seq . code)
  (append-all code))

(define (string obj)
  (cond
   ((string? obj) obj)
   ((symbol? obj) (symbol->string obj))
   (else (throw-error "can't make" obj "a string"))))

(define (update-var-ref ref env)
  (let ((real-var (variable-reference-variable-ref ref)))
    (write-dbg 'update-var-ref 'ref ref 'real real-var 'env env)
    (if (global-variable? real-var)
	ref
	(let ((new-ref (var-in-env? (variable-name-ref real-var) env)))
	  (if (not (null? new-ref))
	      new-ref
	      (when (variable-is-free-ref real-var)
	        (throw-error
		 "ENV: " env
		 new-ref "not in env and it's marked free")))))))

(define (gen-var var env)
  "given VAR, a variable reference, generate the bytecode to access
that variable given that our environment looks like ENV"
  (write-dbg 'gen-var var)
  (let ((real-var (variable-reference-variable-ref var))
	(new-ref (update-var-ref var env)))

    (cond
     ((global-variable? real-var)
      (gen 'gvar (global-variable-name-ref real-var)))

     ;; we did the lookup again to account for skipped frames
     ((variable-is-free-ref real-var)
      (gen 'lvar
	   (variable-reference-frame-ref new-ref)
	   (variable-idx-ref real-var) ";" new-ref))
     (else
      (gen 'spush (variable-idx-ref real-var) -1 ";" real-var)))))

(define (gen-set var env)
  "given VAR, a variable reference, generate the bytecode to set that
variable given that our environment looks like ENV"
  (write-dbg 'gen-set var)
  (let ((real-var (variable-reference-variable-ref var))
	(new-ref (update-var-ref var env)))

    (cond
     ((global-variable? real-var)
      (gen 'gset (global-variable-name-ref real-var)))

     ((variable-is-free-ref real-var)
      (gen 'lset
	   (variable-reference-frame-ref new-ref)
	   (variable-idx-ref real-var) ";" var))

     (else
      (gen 'sset (variable-idx-ref real-var) ";" var)))))

(define (in-env? symbol env)
  (let ((frame (find (lambda (f) (member? symbol f)) env)))
    (if (not frame)
	nil
	(list (index-eq frame env) (index-eq symbol frame)))))

(define (label? obj)
  (symbol? obj))

(define (args instr)
  (if (pair? instr) (rest instr)))

(define (arg1 instr)
  (if (pair? instr) (second instr)))

(define (set-arg1! instr val)
  (set-car! (cdr instr) val))

(define (arg2 instr)
  (if (pair? instr) (third instr)))

(define (is instr op)
  (if (pair? op)
      (member? (opcode instr) op)
      (eq? (opcode instr) op)))

(define (opcode instr)
  (if (label? instr)
      'label
      (first instr)))

(define (instrs-to-bytes instr-vector)
  (let* ((len (vector-length instr-vector))
	 (result (make-bytecode-array (%fixnum-mul 3 len))))

    (dotimes (idx len)
      (let ((instr (vector-ref instr-vector idx))
	    (off (%fixnum-mul idx 3)))

	(bytecode-set! result off (char->integer (opcode instr)))

	(if (cdr instr)
	    (begin
	      (bytecode-set! result (%fixnum-add off 1)
			     (cadr instr))
	      (if (cddr instr)
		  (begin
		    (bytecode-set! result (%fixnum-add off 2)
				   (caddr instr)))
		  ;; no second arg
		  (bytecode-set! result (%fixnum-add off 2) -1)))

	    (begin
	      ;; no first or second arg
	      (bytecode-set! result (%fixnum-add off 1) -1)

	      (bytecode-set! result (%fixnum-add off 2) -1)))))


    result))

(define (build-const-table instrs)
  (let ((result nil)
	(idx 0))

    (dolist (inst instrs)
      (when (is inst '(const fn gvar gset))
        (push! (arg1 inst) result)
	(set-car! (cdr inst) idx)
	(%inc! idx)))

    (apply vector (reverse result))))

(define (assemble fn)
  (let* (;; determine the value of each symbolic label and remove
	 ;; those labels from the instruction stream
	 (r1 (asm-first-pass (fn-code-ref fn)))

	 ;; while everything is still symbolic we extract the consts
	 ;; and mutate the arg of the old instruction to point into
	 ;; the table
	 (consts (build-const-table (fn-code-ref fn)))

	 ;; resolve all jumps and convert the instrs into characters
	 (instrs (asm-second-pass (fn-code-ref fn)
				  (first r1)
				  (second r1)))

	 ;; remember the number of instructions in the stream since
	 ;; the alien byte array doesn't store its length
	 (num-bytes (%fixnum-mul (vector-length instrs) 3))

	 ;; pack the instructions into the final alien byte array
	 (bytes (instrs-to-bytes instrs)))

    ;; pack the final compiled proc
    (make-compiled-proc (list num-bytes bytes consts)
			      (fn-env-ref fn))))


(define (asm-first-pass code)
  (let ((length 0)
	(labels nil))
    (dolist (instr code)
	    (if (label? instr)
		(push! (cons instr length) labels)
		(%inc! length)))
    (list length labels)))

(define (asm-second-pass code length labels)
  (let ((addr 0)
	(code-vector (make-vector length nil)))
    (dolist (instr code)
	    (unless (label? instr)
		    (if (is instr '(jump tjump fjump save))
			(set-arg1! instr
				   (cdr (assoc (arg1 instr) labels))))

		    ;; if this has a bytecode, convert it
		    (let ((bytecode (symbol->bytecode (opcode instr))))
		      (if bytecode
			  (set-car! instr bytecode)))

		    (vector-set! code-vector addr instr)
		    (%inc! addr)))
    code-vector))

(define (fn-opcode? instr)
  (is instr 'fn))

(define (optimize code)
  code)


(define-struct variable
  "maintain information about variable references"
  (name
   is-free
   is-set
   idx))

(define-struct global-variable
  "maintain information about a global variable reference"
  (name))

(define-struct variable-reference
  "a reference to a variable"
  (variable
   frame
   number))

(define (var-in-env? var env)
  (let loop ((frame-number 0)
	     (var-number 0)
	     (frame (car env))
	     (frames (cdr env)))

    (cond
     ((and (null? frame)
	   (null? frames))
      nil) ;; variable not found in environment
     ((null? frame)
      (loop (%fixnum-add frame-number 1)
	    0
	    (car frames)
	    (cdr frames))) ;; move to next frame
     ((eq? (variable-name-ref (car frame)) var)
      (make-variable-reference
       'variable (car frame)
       'frame frame-number
       'number var-number)) ;; found what we're looking for
     (else
      (loop frame-number
	    (%fixnum-add var-number 1)
	    (cdr frame)
	    frames)))))

(define (set-reference-free! ref)
  (variable-is-free-set! (variable-reference-variable-ref ref) #t))

(define (set-reference-set! ref)
  (variable-is-set-set! (variable-reference-variable-ref ref) #t))

(define (args-to-variables arg-list)
  (cond
   ((null? arg-list) nil)
   ((atom? arg-list) (make-variable 'name arg-list))
   (else (cons (make-variable 'name (car arg-list))
	       (args-to-variables (cdr arg-list))))))

(define (variable-usages exp env)
  (cond
   ((symbol? exp)
    ;; if the veriable reference is against a frame other than this
    ;; one then mark the variable as free. If it's not found then
    ;; generate a global reference
    (let ((var (var-in-env? exp env)))
      (cond
       ((null? var)
	(make-variable-reference 'variable (make-global-variable 'name exp)))
       ((%fixnum-greater-than (variable-reference-frame-ref var) 0)
	(set-reference-free! var)
	var)
       ;; it's in our frame, no need to change its markings
       (else var))))
   ((atom? exp) exp)
   (else
    (record-case exp
      (if-compiling (then else)
	(list 'if-compiling
	  (variable-usages then env)
	  (variable-usages else env)))
      (quote (obj) exp)
      (begin exps
        (list* 'begin
	  (map (lambda (exp)
		 (variable-usages exp env)) exps)))
      (set! (sym val)
	;; if the variable reference is against a frame other than
	;; this one, then mark the variable free and set. if it's
	;; against this frame, mark the variable set only. If the
	;; variable isn't found generate a global set
	(let* ((var (var-in-env? sym env))
	       (ref (cond
		     ((null? var)
		      (make-variable-reference 'variable
					       (make-global-variable 'name sym)))
		     ((%fixnum-greater-than (variable-reference-frame-ref var) 0)
		      (set-reference-free! var)
		      (set-reference-set! var)
		      var)
		     (else
		      (set-reference-set! var)
		      var))))
	  (list 'set! ref (variable-usages val env))))
      (if (test then . else)
	  (list 'if
            (variable-usages test env)
	    (variable-usages then env)
	    (variable-usages (car-else else nil) env)))
      (lambda (args . body)
	;; extend the environment with ARGS and then traverse BODY
	(let* ((new-args (args-to-variables args))
	       (new-env (cons (make-true-list new-args) env)))
	  (list* 'lambda
	    new-args
	    (map (lambda (exp)
		   (variable-usages exp new-env)) body))))
      (else
       (if (comp-macro? (first exp))
	   (variable-usages (comp-macroexpand0 exp) env)
	   (map (lambda (exp)
		  (variable-usages exp env)) exp)))))))

(define (any-free-variables? args)
  "#t if any of the variables in the improper list are free"
  (cond
   ((null? args) #f)
   ((atom? args) (variable-is-free-ref args))
   (else
    (if (variable-is-free-ref (car args))
	#t
	(any-free-variables? (cdr args))))))

(define (set-all-variables-free! args)
  "set all variables in the improper list to be free"
  (cond
   ((atom? args)
    (variable-is-free-set! args #t))
   (else
    (variable-is-free-set! (car args) #t)
    (set-all-variables-free! (cdr args)))))


(define (make-space spaces)
  (make-string spaces #\space))

(define (%compiled->instructions fn)
  "produce a stream of readable instructions from compiled bytecode"
  (let ((len (/ (car (compiled-bytecode fn)) 3))
	(bytes (cadr (compiled-bytecode fn)))
	(consts (caddr (compiled-bytecode fn)))
	(result nil))
    (dotimes (idx len)
      (let* ((off (* idx 3))
	     (instr (bytecode-ref bytes off))
	     (arg1 (bytecode-ref bytes (+ off 1)))
	     (arg2 (bytecode-ref bytes (+ off 2)))
	     (instr* (bytecode->symbol (integer->char instr)))
	     (arg1* (if (member instr* '(fn const gvar gset))
			(vector-ref consts arg1)
			arg1)))
	(push! (list instr* arg1* arg2)
	       result)))
    (reverse result)))

(define (%show-fn fn indent)
  (newline)
  (let ((line-num 0))
    (dolist (instr (%compiled->instructions fn))
      (if (is instr 'fn)
	  (begin
	    (map display (list line-num ": " (make-space indent) "fn "))
	    (%show-fn (arg1 instr) (%fixnum-add indent 4)))
	  (begin
	    (map display (list line-num ": " (make-space indent) instr))
	    (newline)))
      (inc! line-num))))


(define (comp-show fn)
  (%show-fn (compiler fn) 0))

(define (dump-compiled-fn fn . indent)
  (let ((indent (if (null? indent)
		    0
		    (car indent))))
    (%show-fn fn indent)))

(define (comp-repl)
  (display "comp-repl> ")
  (let ((result ((compiler (read-port stdin)))))
    (write-port result stdout)
    (newline)
    (unless (eq? result 'quit)
	    (comp-repl))))

; now we can compile functions to bytecode and print the results like
; this:
; (comp-show '(if (= x y) (f (g x)) (h x y (h 1 2))))


(define (compiling-load-eval form env)
  (let ((result ((compiler form))))
    result))

(define (compile-file name)
  "read and compile all forms in file"
  (let ((file (find-library name)))
    (if file
        (letrec ((in (open-input-port file))
                 (iter (lambda (form)
                         (unless (eof-object? form)
                           ((compiler form))
                           (iter (read-port in))))))
          (if (eof-object? in)
              (throw-error "compiler failed to open" file)
              (iter (read-port in)))
          #t)
        (throw-error "failed to find" name))))

(provide 'compiler)


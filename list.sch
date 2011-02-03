(define (plist-get list key)
  "Return property value in plist."
  (second (member key list)))

(define (plist-set! list key value)
  "Set property value in plist."
  (let ((rest (member key list)))
    (set-cdr! rest (cons value (cddr rest)))))

(define (nthcdr n lst)
  "Return nth cdr of list."
  (if (zero? n)
      lst
      (nthcdr (- n 1) (cdr lst))))

(define (make-list len init)
  "Make a new list of length LEN composed of INIT."
  (if (zero? len)
      '()
      (cons init (make-list (- len 1) init))))

(define (copy-list lst)
  "Make a copy of the list, sharing elements."
  (if (null? lst)
      '()
      (cons (car lst) (copy-list (cdr lst)))))

(letrec ((merge (lambda (test key a b)
		   (cond
		    ((null? a) b)
		    ((null? b) a)
		    ((test (key (car b)) (key (car a)))
		     (cons (car b) (merge test key a (cdr b))))
		    (#t (cons (car a) (merge test key (cdr a) b)))))))

  (define (mergesort! lst test key)
    "Destructively mergesort the given list, ascending by test."
    (let ((len (length lst)))
      (if (<= len 1)
	  lst
	  (let* ((left-end (nthcdr (- (/ len 2) 1) lst))
		 (right (cdr left-end)))
	    (set-cdr! left-end '())
	    (merge test key
		   (mergesort! lst test key)
		   (mergesort! right test key)))))))

(define (sort! lst test (key identity))
  "Destructively sort a list, by test."
  (mergesort! lst test key))

(define (sort lst test (key identity))
  "Non-destructively sort a list, by test."
  (mergesort! (copy-list lst) test key))

(define stable-sort sort)

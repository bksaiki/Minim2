;; Core Scheme library, evaluated by `core_lib_load` during `Minit`
;; after the kernel is registered. Brings every kernel binding into
;; the top-level env, then defines R7RS pair accessors that aren't
;; primitives.
;;
;; The verbose `(define name (lambda (...) ...))` form is used
;; throughout — the function-define shorthand will land later as a
;; macro once the macro layer exists.
;;
;; Editing this file triggers a CMake re-bake of `core_lib_data.h`
;; on the next build (CMAKE_CONFIGURE_DEPENDS in CMakeLists.txt).

(import (prefix #%kernel $))

;; --------------------------------------------------------
;; Low-level exceptions

;; TODO: support exports
;; (export error
;;        raise-argument-error)

;; (define error
;;   (lambda (who msg . args)
;;     ($error who msg args)))

;; TODO: need strings to write string message
(define raise-argument-error
  (lambda (who expect v)
    ($error who expect v)))

;; --------------------------------------------------------
;; Pair accessors

(define car
  (lambda (x)
    (if (pair? x)
        ($car x)
        (raise-argument-error 'car "pair" x))))

(define cdr
  (lambda (x)
    (if (pair? x)
        ($cdr x)
        (raise-argument-error 'cdr "pair" x))))

;; -- depth 2 -----------------------------------------------------
(define caar (lambda (p) (car (car p))))
(define cadr (lambda (p) (car (cdr p))))
(define cdar (lambda (p) (cdr (car p))))
(define cddr (lambda (p) (cdr (cdr p))))

;; -- depth 3 -----------------------------------------------------
(define caaar (lambda (p) (car (caar p))))
(define caadr (lambda (p) (car (cadr p))))
(define cadar (lambda (p) (car (cdar p))))
(define caddr (lambda (p) (car (cddr p))))
(define cdaar (lambda (p) (cdr (caar p))))
(define cdadr (lambda (p) (cdr (cadr p))))
(define cddar (lambda (p) (cdr (cdar p))))
(define cdddr (lambda (p) (cdr (cddr p))))

;; -- depth 4 -----------------------------------------------------
(define caaaar (lambda (p) (car (caaar p))))
(define caaadr (lambda (p) (car (caadr p))))
(define caadar (lambda (p) (car (cadar p))))
(define caaddr (lambda (p) (car (caddr p))))
(define cadaar (lambda (p) (car (cdaar p))))
(define cadadr (lambda (p) (car (cdadr p))))
(define caddar (lambda (p) (car (cddar p))))
(define cadddr (lambda (p) (car (cdddr p))))
(define cdaaar (lambda (p) (cdr (caaar p))))
(define cdaadr (lambda (p) (cdr (caadr p))))
(define cdadar (lambda (p) (cdr (cadar p))))
(define cdaddr (lambda (p) (cdr (caddr p))))
(define cddaar (lambda (p) (cdr (cdaar p))))
(define cddadr (lambda (p) (cdr (cdadr p))))
(define cdddar (lambda (p) (cdr (cddar p))))
(define cddddr (lambda (p) (cdr (cdddr p))))

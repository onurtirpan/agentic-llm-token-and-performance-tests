;;;; Dump an SBCL core with the Lisp dependencies already loaded.
;;;;
;;;; Without this, every launch pays Quicklisp for finding, compiling and
;;;; loading Hunchentoot, jzon and cl-ppcre, which is several seconds. No real
;;;; Common Lisp deployment starts that way; the normal practice is to ship an
;;;; image. Charging Lisp for our launch method would be the same class of
;;;; mistake as leaving OPcache off for PHP.
;;;;
;;;; Usage:  sbcl --non-interactive --no-userinit --load tools/build_lisp_core.lisp <out>

(load (merge-pathnames "quicklisp/setup.lisp" (user-homedir-pathname)))
(funcall (read-from-string "ql:quickload")
         '(:hunchentoot :com.inuoe.jzon :cl-ppcre) :silent t)

(let ((target (second sb-ext:*posix-argv*)))
  (unless target
    (format *error-output* "~&usage: ... build_lisp_core.lisp <output.core>~%")
    (sb-ext:exit :code 2))
  (format t "~&writing ~a~%" target)
  (sb-ext:save-lisp-and-die target))

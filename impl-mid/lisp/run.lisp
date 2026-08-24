;;;; Dependency manifest and launcher. Not application source.
(load (merge-pathnames "quicklisp/setup.lisp" (user-homedir-pathname)))
(ql:quickload '(:hunchentoot :com.inuoe.jzon :cl-ppcre) :silent t)
(load (merge-pathnames "main.lisp" *load-truename*))

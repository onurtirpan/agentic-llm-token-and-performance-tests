;;;; Dependency manifest and launcher. Not application source.
(load (merge-pathnames "quicklisp/setup.lisp" (user-homedir-pathname)))
(ql:quickload '(:hunchentoot :com.inuoe.jzon :cl-ppcre) :silent t)
(dolist (file '("domain.lisp" "store.lisp" "service.lisp" "api.lisp"))
  (load (merge-pathnames file *load-truename*)))

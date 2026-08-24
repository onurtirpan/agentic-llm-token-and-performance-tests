;;;; Task Service — Hunchentoot implementation.

(defpackage :task-service
  (:use :cl))
(in-package :task-service)

(defconstant +max-title-length+ 80)
(defconstant +min-priority+ 1)
(defconstant +max-priority+ 5)
(defconstant +port+ 8080)

(defstruct task id title priority done score)

(defvar *tasks* (make-hash-table))
(defvar *next-id* 1)

(defun compute-score (priority done)
  (let ((base-score (* priority 10)))
    (if done base-score (+ base-score 5))))

(defun validate (title priority)
  (cond ((zerop (length title)) "title is required")
        ((> (length title) +max-title-length+) "title is too long")
        ((or (< priority +min-priority+) (> priority +max-priority+))
         "priority is out of range")
        (t nil)))

(defun object (&rest pairs)
  "Build the hash table that jzon stringifies into a JSON object."
  (let ((table (make-hash-table :test #'equal :size (length pairs))))
    (loop for (key value) on pairs by #'cddr do (setf (gethash key table) value))
    table))

(defun task-json (task)
  (object "id" (task-id task) "title" (task-title task)
          "priority" (task-priority task) "done" (task-done task)
          "score" (task-score task)))

(defun respond (status body)
  (setf (hunchentoot:return-code*) status
        (hunchentoot:content-type*) "application/json")
  (com.inuoe.jzon:stringify body))

(defun fail (status message)
  (respond status (object "error" message)))

(defun parse-id (raw)
  (multiple-value-bind (value end) (parse-integer raw :junk-allowed t)
    (and value (= end (length raw)) value)))

(defun read-input (body)
  "Return (values title priority done), or NIL when the body is not usable."
  (handler-case
      (let ((parsed (com.inuoe.jzon:parse body)))
        (unless (hash-table-p parsed) (return-from read-input nil))
        (let ((title (gethash "title" parsed ""))
              (priority (gethash "priority" parsed 0))
              (done (gethash "done" parsed nil)))
          (unless (and (stringp title) (integerp priority)
                       (or (eq done t) (eq done nil)))
            (return-from read-input nil))
          (values title priority done)))
    (error () nil)))

(defun sorted-tasks ()
  (let ((all '()))
    (maphash (lambda (id task) (declare (ignore id)) (push task all)) *tasks*)
    (sort all #'< :key #'task-id)))

(defun get-health ()
  (respond 200 (object "status" "ok" "count" (hash-table-count *tasks*))))

(defun list-tasks ()
  (let ((done (hunchentoot:get-parameter "done")))
    (if (and done (not (string= done "true")) (not (string= done "false")))
        (fail 400 "done must be true or false")
        (let ((selected (remove-if-not
                         (lambda (task)
                           (or (null done)
                               (eq (and (task-done task) t) (string= done "true"))))
                         (sorted-tasks))))
          ;; The list is already in ascending id order, and stable-sort keeps
          ;; that order inside a run of equal scores. So this is score
          ;; descending, then id ascending, in one pass.
          (setf selected (stable-sort selected #'> :key #'task-score))
          (respond 200 (object "tasks" (map 'vector #'task-json selected)
                               "total" (length selected)))))))

(defun get-task (raw-id)
  (let ((id (parse-id raw-id)))
    (cond ((null id) (fail 400 "invalid id"))
          ((null (gethash id *tasks*)) (fail 404 "task not found"))
          (t (respond 200 (task-json (gethash id *tasks*)))))))

(defun create-task ()
  (multiple-value-bind (title priority)
      (read-input (hunchentoot:raw-post-data :force-text t))
    (if (null title)
        (fail 400 "invalid json")
        (let ((error (validate title priority)))
          (if error
              (fail 400 error)
              (let ((task (make-task :id *next-id* :title title :priority priority
                                     :done nil :score (compute-score priority nil))))
                (setf (gethash *next-id* *tasks*) task)
                (incf *next-id*)
                (respond 201 (task-json task))))))))

(defun update-task (raw-id)
  (let ((id (parse-id raw-id)))
    (cond
      ((null id) (fail 400 "invalid id"))
      ((null (gethash id *tasks*)) (fail 404 "task not found"))
      (t (multiple-value-bind (title priority done)
             (read-input (hunchentoot:raw-post-data :force-text t))
           (if (null title)
               (fail 400 "invalid json")
               (let ((error (validate title priority)))
                 (if error
                     (fail 400 error)
                     (let ((task (gethash id *tasks*)))
                       (setf (task-title task) title
                             (task-priority task) priority
                             (task-done task) done
                             (task-score task) (compute-score priority done))
                       (respond 200 (task-json task)))))))))))

(defun delete-task (raw-id)
  (let ((id (parse-id raw-id)))
    (cond ((null id) (fail 400 "invalid id"))
          ((null (remhash id *tasks*)) (fail 404 "task not found"))
          (t (setf (hunchentoot:return-code*) 204)
             ""))))

(defun get-stats ()
  (let* ((all (sorted-tasks))
         (total (length all))
         (done-count (count-if #'task-done all))
         (sum-score (reduce #'+ all :key #'task-score :initial-value 0))
         (avg-score (if (zerop total)
                        0
                        (/ (fround (/ (* 100.0d0 sum-score) total)) 100)))
         (best nil))
    (dolist (task all)
      (unless (task-done task)
        (when (or (null best) (> (task-priority task) (task-priority best)))
          (setf best task))))
    (respond 200 (object "total" total "doneCount" done-count
                         "openCount" (- total done-count)
                         "avgScore" avg-score
                         "topOpenTitle" (if best (task-title best) :null)))))

(defparameter *routes*
  (list (list :get "^/health$" (lambda () (get-health)))
        (list :get "^/tasks$" (lambda () (list-tasks)))
        (list :get "^/tasks/([^/]+)$" #'get-task)
        (list :post "^/tasks$" (lambda () (create-task)))
        (list :put "^/tasks/([^/]+)$" #'update-task)
        (list :delete "^/tasks/([^/]+)$" #'delete-task)
        (list :get "^/stats$" (lambda () (get-stats)))))

(defun dispatch ()
  (let ((method (hunchentoot:request-method*))
        (path (hunchentoot:script-name*)))
    (loop for (verb pattern handler) in *routes* do
      (when (eq verb method)
        (multiple-value-bind (match groups) (cl-ppcre:scan-to-strings pattern path)
          (when match
            (return-from dispatch (apply handler (coerce groups 'list)))))))
    (fail 404 "not found")))

(defvar *server*
  (make-instance 'hunchentoot:easy-acceptor
                 :port +port+
                 :address "127.0.0.1"
                 :access-log-destination nil
                 :message-log-destination nil))

(setf hunchentoot:*dispatch-table*
      (list (lambda (request) (declare (ignore request)) #'dispatch)))
(setf hunchentoot:*show-lisp-errors-p* nil)

(hunchentoot:start *server*)
(loop (sleep 3600))

;;;; Task Service, mid tier — Hunchentoot implementation.

(defpackage :task-service
  (:use :cl))
(in-package :task-service)

(defconstant +max-title-length+ 80)
(defconstant +max-name-length+ 60)
(defconstant +min-priority+ 1)
(defconstant +max-priority+ 5)
(defconstant +default-limit+ 20)
(defconstant +max-limit+ 100)
(defconstant +port+ 8080)

(defparameter *status-bonus*
  '(("todo" . 0) ("in_progress" . 3) ("done" . 5) ("archived" . 0)))
(defparameter *transitions*
  '(("todo" . "in_progress") ("todo" . "archived") ("in_progress" . "todo")
    ("in_progress" . "done") ("done" . "archived")))
(defparameter *project-sorts* '("id" "name" "taskCount"))
(defparameter *task-sorts* '("id" "title" "priority" "score" "status"))

(defstruct user id username password role)
(defstruct project id name owner-id)
(defstruct task id project-id title priority status assignee-id score)

(define-condition app-error (error)
  ((status :initarg :status :reader app-error-status)
   (code :initarg :code :reader app-error-code)
   (message :initarg :message :reader app-error-message)
   (details :initarg :details :initform '() :reader app-error-details)))

(defvar *users*
  (let ((table (make-hash-table)))
    (dolist (user (list (make-user :id 1 :username "admin" :password "admin-secret"
                                   :role "admin")
                        (make-user :id 2 :username "alice" :password "alice-secret"
                                   :role "user")
                        (make-user :id 3 :username "bob" :password "bob-secret"
                                   :role "user"))
                  table)
      (setf (gethash (user-id user) table) user))))
(defvar *sessions* (make-hash-table :test #'equal))
(defvar *projects* (make-hash-table))
(defvar *tasks* (make-hash-table))
(defvar *next-project-id* 1)
(defvar *next-task-id* 1)

(defvar *request-id* nil)
(defvar *user-id* nil)
(defvar *errors* '()
  "The validation details of the request in flight. The READ- functions push
onto it instead of returning early, so one response reports every broken rule.")

(defun object (&rest pairs)
  "Build the hash table that jzon stringifies into a JSON object."
  (let ((table (make-hash-table :test #'equal :size (length pairs))))
    (loop for (key value) on pairs by #'cddr do (setf (gethash key table) value))
    table))

(defun fail (field message)
  (cons field message))

(defun invalid (details)
  (error 'app-error :status 422 :code "validation_failed"
                    :message "the request body is not valid"
                    :details (sort details
                                   (lambda (left right)
                                     (if (string= (car left) (car right))
                                         (string< (cdr left) (cdr right))
                                         (string< (car left) (car right)))))))

(defun bad-request ()
  (error 'app-error :status 400 :code "bad_request"
                    :message "the request is malformed"))

(defun forbidden ()
  (error 'app-error :status 403 :code "forbidden"
                    :message "you may not access this resource"))

(defun not-found ()
  (error 'app-error :status 404 :code "not_found"
                    :message "the resource does not exist"))

(defun conflict ()
  (error 'app-error :status 409 :code "conflict"
                    :message "the resource already exists"))

(defun sorted-rows (table key)
  (let ((rows '()))
    (maphash (lambda (id row) (declare (ignore id)) (push row rows)) table)
    (sort rows #'< :key key)))

(defun all-users ()
  (sorted-rows *users* #'user-id))

(defun all-projects ()
  (sorted-rows *projects* #'project-id))

(defun all-tasks ()
  (sorted-rows *tasks* #'task-id))

(defun compute-score (priority status)
  (let ((base-score (* priority 10)))
    (+ base-score (cdr (assoc status *status-bonus* :test #'string=)))))

(defun task-count (project-id)
  (count-if (lambda (task) (= (task-project-id task) project-id)) (all-tasks)))

(defun serialize-project (project)
  (object "id" (project-id project) "name" (project-name project)
          "ownerId" (project-owner-id project)
          "taskCount" (task-count (project-id project))))

(defun serialize-task (task)
  (object "id" (task-id task) "projectId" (task-project-id task)
          "title" (task-title task) "priority" (task-priority task)
          "status" (task-status task) "assigneeId" (task-assignee-id task)
          "score" (task-score task)))

(defun respond (status body)
  (setf (hunchentoot:return-code*) status
        (hunchentoot:content-type*) "application/json")
  (com.inuoe.jzon:stringify body))

(defun respond-no-content ()
  (setf (hunchentoot:return-code*) 204)
  "")

(defun respond-app-error (failure)
  (respond (app-error-status failure)
           (object "error"
                   (object "code" (app-error-code failure)
                           "message" (app-error-message failure)
                           "requestId" *request-id*
                           "details" (map 'vector
                                          (lambda (detail)
                                            (object "field" (car detail)
                                                    "message" (cdr detail)))
                                          (app-error-details failure))))))

(defun read-body ()
  (let ((raw (hunchentoot:raw-post-data :force-text t)))
    (when (or (null raw) (string= (string-trim '(#\Space #\Tab #\Return #\Newline) raw) ""))
      (return-from read-body (object)))
    (let ((parsed (handler-case (com.inuoe.jzon:parse raw)
                    (com.inuoe.jzon:json-error () (bad-request)))))
      (unless (hash-table-p parsed)
        (bad-request))
      parsed)))

(defun read-int (body field default)
  "The integer at FIELD, its default, or the symbol NULL that jzon reads a JSON
null as. Any other type makes the whole request malformed."
  (let ((value (gethash field body default)))
    (if (or (eq value 'null) (integerp value))
        value
        (bad-request))))

(defun read-string (body field max-length required)
  (let ((value (gethash field body "")))
    (unless (stringp value)
      (bad-request))
    (cond ((string= value "")
           (when required
             (push (fail field (format nil "~a is required" field)) *errors*)))
          ((> (length value) max-length)
           (push (fail field (format nil "~a is too long" field)) *errors*)))
    value))

(defun read-priority (body)
  (let ((value (read-int body "priority" 0)))
    (when (or (eq value 'null) (< value +min-priority+) (> value +max-priority+))
      (push (fail "priority" "priority is out of range") *errors*))
    (if (eq value 'null) 0 value)))

(defun read-user-ref (body field default nullable)
  (let ((value (read-int body field default)))
    (if (and (eq value 'null) nullable)
        'null
        (progn
          (unless (gethash value *users*)
            (push (fail field (format nil "~a is not a known user" field)) *errors*))
          value))))

(defun parse-whole-integer (raw)
  (multiple-value-bind (value end) (parse-integer raw :junk-allowed t)
    (and value (= end (length raw)) value)))

(defun parse-id (raw)
  (or (parse-whole-integer raw) (bad-request)))

(defun read-page (allowed)
  (let ((*errors* '())
        (limit +default-limit+)
        (offset 0)
        (sort (or (hunchentoot:get-parameter "sort") "id"))
        (order (or (hunchentoot:get-parameter "order") "asc"))
        (raw-limit (hunchentoot:get-parameter "limit"))
        (raw-offset (hunchentoot:get-parameter "offset")))
    (when raw-limit
      (setf limit (or (parse-whole-integer raw-limit) -1))
      (when (or (< limit 1) (> limit +max-limit+))
        (push (fail "limit" "limit is out of range") *errors*)))
    (when raw-offset
      (setf offset (or (parse-whole-integer raw-offset) -1))
      (when (< offset 0)
        (push (fail "offset" "offset is out of range") *errors*)))
    (unless (member sort allowed :test #'string=)
      (push (fail "sort" "sort is not a valid field") *errors*))
    (unless (or (string= order "asc") (string= order "desc"))
      (push (fail "order" "order must be asc or desc") *errors*))
    (when *errors*
      (invalid *errors*))
    (values limit offset sort order)))

(defun value< (left right)
  (if (stringp left) (string< left right) (< left right)))

(defun value> (left right)
  (value< right left))

(defun paginate (rows limit offset sort order)
  (let* ((by-id (stable-sort rows #'< :key (lambda (row) (gethash "id" row))))
         (sorted (stable-sort by-id (if (string= order "desc") #'value> #'value<)
                              :key (lambda (row) (gethash sort row))))
         (total (length sorted))
         (window (subseq sorted (min offset total) (min (+ offset limit) total))))
    (object "items" (coerce window 'vector) "total" total
            "limit" limit "offset" offset)))

(defun bearer-token ()
  (let ((header (or (hunchentoot:header-in* :authorization) "")))
    (when (and (>= (length header) 7) (string= "Bearer " header :end2 7))
      (subseq header 7))))

(defun authenticate ()
  (let* ((token (bearer-token))
         (session (and token (gethash token *sessions*))))
    (unless session
      (error 'app-error :status 401 :code "unauthorized"
                        :message "authentication is required"))
    (setf *user-id* session)
    (gethash session *users*)))

(defun require-admin (user)
  (unless (string= (user-role user) "admin")
    (forbidden)))

(defun reachable-project (project-id user)
  (let ((project (gethash project-id *projects*)))
    (unless project
      (not-found))
    (when (and (string/= (user-role user) "admin")
               (/= (project-owner-id project) (user-id user)))
      (forbidden))
    project))

(defun reachable-task (task-id user)
  (let ((task (gethash task-id *tasks*)))
    (unless task
      (not-found))
    (reachable-project (task-project-id task) user)
    task))

(defun opaque-id (length)
  (format nil "~(~v,'0x~)" length (random (expt 16 length))))

(defun get-health ()
  (respond 200 (object "status" "ok"
                       "projects" (hash-table-count *projects*)
                       "tasks" (hash-table-count *tasks*))))

(defun login ()
  (let* ((body (read-body))
         (*errors* '())
         (username (read-string body "username" +max-name-length+ t))
         (password (read-string body "password" +max-name-length+ t)))
    (when *errors*
      (invalid *errors*))
    (let ((user (find-if (lambda (candidate)
                           (and (string= (user-username candidate) username)
                                (string= (user-password candidate) password)))
                         (all-users))))
      (unless user
        (error 'app-error :status 401 :code "invalid_credentials"
                          :message "the username or password is wrong"))
      (let ((token (opaque-id 32)))
        (setf (gethash token *sessions*) (user-id user))
        (respond 200 (object "token" token "userId" (user-id user)
                             "role" (user-role user)))))))

(defun logout ()
  (authenticate)
  (remhash (bearer-token) *sessions*)
  (respond-no-content))

(defun get-me ()
  (let ((user (authenticate)))
    (respond 200 (object "userId" (user-id user) "username" (user-username user)
                         "role" (user-role user)))))

(defun list-projects ()
  (let ((user (authenticate)))
    (multiple-value-bind (limit offset sort order) (read-page *project-sorts*)
      (let ((rows (mapcar #'serialize-project
                          (remove-if-not
                           (lambda (project)
                             (or (string= (user-role user) "admin")
                                 (= (project-owner-id project) (user-id user))))
                           (all-projects)))))
        (respond 200 (paginate rows limit offset sort order))))))

(defun create-project ()
  (let ((user (authenticate)))
    (require-admin user)
    (let* ((body (read-body))
           (*errors* '())
           (name (read-string body "name" +max-name-length+ t))
           (owner-id (read-user-ref body "ownerId" (user-id user) nil)))
      (when *errors*
        (invalid *errors*))
      (when (find-if (lambda (other)
                       (and (= (project-owner-id other) owner-id)
                            (string= (project-name other) name)))
                     (all-projects))
        (conflict))
      (let ((project (make-project :id *next-project-id* :name name :owner-id owner-id)))
        (setf (gethash *next-project-id* *projects*) project)
        (incf *next-project-id*)
        (respond 201 (serialize-project project))))))

(defun get-project (raw-id)
  (let ((user (authenticate)))
    (respond 200 (serialize-project (reachable-project (parse-id raw-id) user)))))

(defun update-project (raw-id)
  (let ((user (authenticate)))
    (require-admin user)
    (let ((project (reachable-project (parse-id raw-id) user))
          (body (read-body)))
      (unless (nth-value 1 (gethash "name" body))
        (return-from update-project (respond 200 (serialize-project project))))
      (let* ((*errors* '())
             (name (read-string body "name" +max-name-length+ t)))
        (when *errors*
          (invalid *errors*))
        (when (find-if (lambda (other)
                         (and (= (project-owner-id other) (project-owner-id project))
                              (string= (project-name other) name)
                              (/= (project-id other) (project-id project))))
                       (all-projects))
          (conflict))
        (setf (project-name project) name)
        (respond 200 (serialize-project project))))))

(defun delete-project (raw-id)
  (let ((user (authenticate)))
    (require-admin user)
    (let ((project (reachable-project (parse-id raw-id) user)))
      (dolist (task (all-tasks))
        (when (= (task-project-id task) (project-id project))
          (remhash (task-id task) *tasks*)))
      (remhash (project-id project) *projects*)
      (respond-no-content))))

(defun list-tasks (raw-id)
  (let* ((user (authenticate))
         (project (reachable-project (parse-id raw-id) user)))
    (multiple-value-bind (limit offset sort order) (read-page *task-sorts*)
      (let ((rows (mapcar #'serialize-task
                          (remove-if-not (lambda (task)
                                           (= (task-project-id task) (project-id project)))
                                         (all-tasks)))))
        (respond 200 (paginate rows limit offset sort order))))))

(defun create-task (raw-id)
  (let* ((user (authenticate))
         (project (reachable-project (parse-id raw-id) user))
         (body (read-body))
         (*errors* '())
         (title (read-string body "title" +max-title-length+ t))
         (priority (read-priority body))
         (assignee-id (read-user-ref body "assigneeId" 'null t)))
    (when *errors*
      (invalid *errors*))
    (let ((task (make-task :id *next-task-id* :project-id (project-id project)
                           :title title :priority priority :status "todo"
                           :assignee-id assignee-id
                           :score (compute-score priority "todo"))))
      (setf (gethash *next-task-id* *tasks*) task)
      (incf *next-task-id*)
      (respond 201 (serialize-task task)))))

(defun get-task (raw-id)
  (let ((user (authenticate)))
    (respond 200 (serialize-task (reachable-task (parse-id raw-id) user)))))

(defun replace-task (raw-id)
  (let* ((user (authenticate))
         (task (reachable-task (parse-id raw-id) user))
         (body (read-body))
         (*errors* '())
         (title (read-string body "title" +max-title-length+ t))
         (priority (read-priority body))
         (assignee-id (read-user-ref body "assigneeId" 'null t)))
    (when *errors*
      (invalid *errors*))
    (setf (task-title task) title
          (task-priority task) priority
          (task-assignee-id task) assignee-id
          (task-score task) (compute-score priority (task-status task)))
    (respond 200 (serialize-task task))))

(defun update-status (raw-id)
  (let* ((user (authenticate))
         (task (reachable-task (parse-id raw-id) user))
         (body (read-body))
         (status (gethash "status" body)))
    (unless (and (stringp status) (assoc status *status-bonus* :test #'string=))
      (invalid (list (fail "status" "status is not valid"))))
    (unless (member (cons (task-status task) status) *transitions* :test #'equal)
      (error 'app-error :status 409 :code "invalid_transition"
                        :message "the status change is not allowed"))
    (setf (task-status task) status
          (task-score task) (compute-score (task-priority task) status))
    (respond 200 (serialize-task task))))

(defun delete-task (raw-id)
  (let* ((user (authenticate))
         (task (reachable-task (parse-id raw-id) user)))
    (remhash (task-id task) *tasks*)
    (respond-no-content)))

(defun get-stats ()
  (let ((user (authenticate)))
    (require-admin user)
    (let* ((all (all-tasks))
           (total (length all))
           (sum-score (reduce #'+ all :key #'task-score :initial-value 0))
           (avg-score (if (zerop total)
                          0.0d0
                          (/ (fround (/ (* 100.0d0 sum-score) total)) 100)))
           (by-status (object))
           (best nil))
      (dolist (entry *status-bonus*)
        (setf (gethash (car entry) by-status) 0))
      (dolist (task all)
        (incf (gethash (task-status task) by-status)))
      (dolist (project (all-projects))
        (when (or (null best)
                  (> (task-count (project-id project)) (task-count (project-id best))))
          (setf best project)))
      (respond 200 (object "projects" (hash-table-count *projects*)
                           "tasks" total
                           "users" (hash-table-count *users*)
                           "sessions" (hash-table-count *sessions*)
                           "byStatus" by-status
                           "avgScore" avg-score
                           "topProjectName" (if best (project-name best) 'null))))))

(defparameter *routes*
  (list (list :get "^/health$" #'get-health)
        (list :post "^/auth/login$" #'login)
        (list :post "^/auth/logout$" #'logout)
        (list :get "^/me$" #'get-me)
        (list :get "^/projects$" #'list-projects)
        (list :post "^/projects$" #'create-project)
        (list :get "^/projects/([^/]+)$" #'get-project)
        (list :patch "^/projects/([^/]+)$" #'update-project)
        (list :delete "^/projects/([^/]+)$" #'delete-project)
        (list :get "^/projects/([^/]+)/tasks$" #'list-tasks)
        (list :post "^/projects/([^/]+)/tasks$" #'create-task)
        (list :get "^/tasks/([^/]+)$" #'get-task)
        (list :put "^/tasks/([^/]+)$" #'replace-task)
        (list :patch "^/tasks/([^/]+)/status$" #'update-status)
        (list :delete "^/tasks/([^/]+)$" #'delete-task)
        (list :get "^/stats$" #'get-stats)))

(defun route ()
  (let ((method (hunchentoot:request-method*))
        (path (hunchentoot:script-name*)))
    (loop for (verb pattern handler) in *routes* do
      (when (eq verb method)
        (multiple-value-bind (match groups) (cl-ppcre:scan-to-strings pattern path)
          (when match
            (return-from route (apply handler (coerce groups 'list)))))))
    (not-found)))

(defun log-request (status started)
  "Write the one line the request owes to stdout. The jzon writer is here for
the key order that a hash table does not promise."
  (let ((line (with-output-to-string (out)
                (com.inuoe.jzon:with-writer* (:stream out)
                  (com.inuoe.jzon:write-object*
                   "level" (cond ((>= status 500) "error")
                                 ((>= status 400) "warn")
                                 (t "info"))
                   "requestId" *request-id*
                   "method" (symbol-name (hunchentoot:request-method*))
                   "path" (hunchentoot:script-name*)
                   "status" status
                   "durationMs" (floor (* 1000 (- (get-internal-real-time) started))
                                       internal-time-units-per-second)
                   "userId" (or *user-id* 'null))))))
    (write-line line)
    (force-output)))

(defun dispatch ()
  (let* ((sent (hunchentoot:header-in* :x-request-id))
         (*request-id* (if (and sent (string/= sent "")) sent (opaque-id 12)))
         (*user-id* nil)
         (started (get-internal-real-time)))
    (setf (hunchentoot:header-out :x-request-id) *request-id*)
    (let ((body (handler-case (route)
                  (app-error (failure) (respond-app-error failure)))))
      (log-request (hunchentoot:return-code*) started)
      body)))

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

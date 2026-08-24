;;;; Task Service, large tier — domain types, constants and pure rules.

(defpackage :task-domain
  (:use :cl)
  (:export #:+max-title-length+ #:+max-name-length+ #:+max-comment-length+
           #:+max-bulk-items+ #:+min-priority+ #:+max-priority+ #:+default-limit+
           #:+max-limit+ #:+default-quota+ #:+probe-quota+ #:+port+
           #:+roles+ #:+statuses+ #:+status-bonus+ #:+transitions+ #:+actions+
           #:+project-sorts+ #:+task-sorts+ #:+user-sorts+ #:+comment-sorts+
           #:+seq-sorts+ #:+group-bys+
           #:make-user #:user-id #:user-username #:user-password #:user-role
           #:user-quota #:user-version #:user-deleted
           #:make-session #:session-token #:session-user-id #:session-used
           #:make-project #:project-id #:project-name #:project-owner-id
           #:project-version #:project-deleted
           #:make-task #:task-id #:task-project-id #:task-title #:task-priority
           #:task-status #:task-assignee-id #:task-internal-note #:task-version
           #:task-deleted
           #:make-comment #:comment-id #:comment-task-id #:comment-author-id
           #:comment-body
           #:make-audit-entry #:audit-entry-seq #:audit-entry-actor-id
           #:audit-entry-action #:audit-entry-resource #:audit-entry-resource-id
           #:make-outbox-event #:outbox-event-seq #:outbox-event-name
           #:outbox-event-resource-id #:outbox-event-delivered
           #:app-error #:app-error-status #:app-error-code #:app-error-message
           #:app-error-details
           #:bad-request #:unauthorized #:invalid-credentials #:forbidden
           #:not-found #:conflict #:invalid-transition #:precondition-failed
           #:precondition-required #:quota-exceeded #:invalid #:fail
           #:compute-score #:check-string #:check-priority #:check-status
           #:check-role #:check-quota #:object #:has-field))

(in-package :task-domain)

(defconstant +max-title-length+ 80)
(defconstant +max-name-length+ 60)
(defconstant +max-comment-length+ 200)
(defconstant +max-bulk-items+ 20)
(defconstant +min-priority+ 1)
(defconstant +max-priority+ 5)
(defconstant +default-limit+ 20)
(defconstant +max-limit+ 100)
(defconstant +default-quota+ 10000)
(defconstant +probe-quota+ 5)
(defconstant +port+ 8080)

;; DEFCONSTANT may not hold a freshly consed list, so the tables below use
;; DEFPARAMETER while keeping the constant naming convention.
(defparameter +roles+ '("admin" "user"))
(defparameter +statuses+ '("todo" "in_progress" "done" "archived"))
(defparameter +status-bonus+
  '(("todo" . 0) ("in_progress" . 3) ("done" . 5) ("archived" . 0)))
(defparameter +transitions+
  '(("todo" . "in_progress") ("todo" . "archived") ("in_progress" . "todo")
    ("in_progress" . "done") ("done" . "archived")))
(defparameter +actions+ '("create" "update" "delete" "restore"))
(defparameter +project-sorts+ '("id" "name" "taskCount"))
(defparameter +task-sorts+ '("id" "title" "priority" "score" "status"))
(defparameter +user-sorts+ '("id" "username" "role"))
(defparameter +comment-sorts+ '("id" "authorId"))
(defparameter +seq-sorts+ '("seq"))
(defparameter +group-bys+ '("assignee" "status" "project"))

(defstruct user
  id username password role quota (version 1) (deleted nil))

(defstruct session
  token user-id (used 0))

(defstruct project
  id name owner-id (version 1) (deleted nil))

(defstruct task
  id project-id title priority status assignee-id
  (internal-note "") (version 1) (deleted nil))

(defstruct comment
  id task-id author-id body)

(defstruct audit-entry
  seq actor-id action resource resource-id)

(defstruct outbox-event
  seq name resource-id (delivered nil))

(define-condition app-error (error)
  ((status :initarg :status :reader app-error-status)
   (code :initarg :code :reader app-error-code)
   (message :initarg :message :reader app-error-message)
   (details :initarg :details :initform '() :reader app-error-details))
  (:documentation "Every failure path signals this. The api layer turns it into the envelope."))

(defun bad-request ()
  (error 'app-error :status 400 :code "bad_request"
                    :message "the request is malformed"))

(defun unauthorized ()
  (error 'app-error :status 401 :code "unauthorized"
                    :message "authentication is required"))

(defun invalid-credentials ()
  (error 'app-error :status 401 :code "invalid_credentials"
                    :message "the username or password is wrong"))

(defun forbidden ()
  (error 'app-error :status 403 :code "forbidden"
                    :message "you may not access this resource"))

(defun not-found ()
  (error 'app-error :status 404 :code "not_found"
                    :message "the resource does not exist"))

(defun conflict ()
  (error 'app-error :status 409 :code "conflict"
                    :message "the resource already exists"))

(defun invalid-transition ()
  (error 'app-error :status 409 :code "invalid_transition"
                    :message "the status change is not allowed"))

(defun precondition-failed ()
  (error 'app-error :status 412 :code "precondition_failed"
                    :message "the resource has changed"))

(defun precondition-required ()
  (error 'app-error :status 428 :code "precondition_required"
                    :message "the If-Match header is required"))

(defun quota-exceeded ()
  (error 'app-error :status 429 :code "quota_exceeded"
                    :message "the request quota is exhausted"))

(defun invalid (details)
  (error 'app-error :status 422 :code "validation_failed"
                    :message "the request body is not valid"
                    :details (sort (copy-list details)
                                   (lambda (left right)
                                     (if (string= (car left) (car right))
                                         (string< (cdr left) (cdr right))
                                         (string< (car left) (car right)))))))

(defun fail (field message)
  "One validation detail. The api layer turns the pair into a JSON object."
  (cons field message))

(defun compute-score (priority status)
  (let ((base-score (* priority 10)))
    (+ base-score (cdr (assoc status +status-bonus+ :test #'string=)))))

(defun check-string (value field-name max-length)
  "Return one detail when the string breaks its rule, otherwise NIL."
  (cond ((zerop (length value))
         (fail field-name (format nil "~A is required" field-name)))
        ((> (length value) max-length)
         (fail field-name (format nil "~A is too long" field-name)))))

(defun check-priority (value)
  (unless (and (integerp value) (<= +min-priority+ value +max-priority+))
    (fail "priority" "priority is out of range")))

(defun check-status (value)
  (unless (and (stringp value) (member value +statuses+ :test #'string=))
    (fail "status" "status is not valid")))

(defun check-role (value)
  (unless (and (stringp value) (member value +roles+ :test #'string=))
    (fail "role" "role is not valid")))

(defun check-quota (value)
  (unless (and (integerp value) (>= value 0))
    (fail "quota" "quota is out of range")))

(defun object (&rest pairs)
  "Build the string-keyed hash table that stands for one JSON object."
  (let ((table (make-hash-table :test #'equal :size (max (length pairs) 1))))
    (loop for (key value) on pairs by #'cddr do (setf (gethash key table) value))
    table))

(defun has-field (body field)
  "True when the parsed body carries the key, even with a false or null value."
  (nth-value 1 (gethash field body)))

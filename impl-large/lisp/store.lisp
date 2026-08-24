;;;; Task Service, large tier — the in-memory state and its repositories.

(defpackage :task-store
  (:use :cl :task-domain)
  (:export #:*users* #:*sessions* #:*projects* #:*tasks* #:*comments* #:*audit*
           #:*outbox* #:*by-status* #:*by-route* #:*requests* #:*state-lock*
           #:seed #:record #:audit-entries #:outbox-events #:count-request
           #:all-users #:all-projects #:all-tasks #:all-comments
           #:find-user #:find-by-username #:insert-user
           #:find-session #:insert-session #:remove-session
           #:find-project #:insert-project #:find-task #:insert-task
           #:find-comment #:insert-comment #:delete-comment
           #:live-tasks-of #:task-count #:outbox-pending
           #:find-idempotent #:record-idempotent))

(in-package :task-store)

(defvar *users* (make-hash-table))
(defvar *sessions* (make-hash-table :test #'equal))
(defvar *projects* (make-hash-table))
(defvar *tasks* (make-hash-table))
(defvar *comments* (make-hash-table))
(defvar *audit* '())
(defvar *outbox* '())
(defvar *idempotency* (make-hash-table :test #'equal))
(defvar *by-status* (make-hash-table))
(defvar *by-route* (make-hash-table :test #'equal))

;; Hunchentoot serves every connection on its own thread, so one lock guards the
;; whole of the state above. The api layer holds it for the length of a request.
(defvar *state-lock* (bordeaux-threads:make-lock "task-service-state"))

(defvar *requests* 0)
(defvar *next-project-id* 1)
(defvar *next-task-id* 1)
(defvar *next-comment-id* 1)
(defvar *next-user-id* 5)
(defvar *next-seq* 1)

(defun seed ()
  (dolist (user (list (make-user :id 1 :username "admin" :password "admin-secret"
                                 :role "admin" :quota +default-quota+)
                      (make-user :id 2 :username "alice" :password "alice-secret"
                                 :role "user" :quota +default-quota+)
                      (make-user :id 3 :username "bob" :password "bob-secret"
                                 :role "user" :quota +default-quota+)
                      (make-user :id 4 :username "probe" :password "probe-secret"
                                 :role "user" :quota +probe-quota+)))
    (setf (gethash (user-id user) *users*) user)))

(defun take-seq ()
  (prog1 *next-seq*
    (incf *next-seq*)))

(defun record (actor-id action resource resource-id)
  "Append one audit entry and one outbox event for a successful write."
  (push (make-audit-entry :seq (take-seq) :actor-id actor-id :action action
                          :resource resource :resource-id resource-id)
        *audit*)
  (push (make-outbox-event :seq (take-seq) :name (format nil "~A.~A" resource action)
                           :resource-id resource-id)
        *outbox*))

(defun audit-entries ()
  (reverse *audit*))

(defun outbox-events ()
  (reverse *outbox*))

(defun count-request (route status)
  (incf *requests*)
  (incf (gethash route *by-route* 0))
  (incf (gethash status *by-status* 0)))

(defun values-of (table)
  (loop for value being the hash-values of table collect value))

;; Every iteration runs in ascending id order, which a hash table does not keep.
(defun all-users ()
  (sort (values-of *users*) #'< :key #'user-id))

(defun all-projects ()
  (sort (values-of *projects*) #'< :key #'project-id))

(defun all-tasks ()
  (sort (values-of *tasks*) #'< :key #'task-id))

(defun all-comments ()
  (sort (values-of *comments*) #'< :key #'comment-id))

(defun find-user (user-id &optional include-deleted)
  (let ((user (gethash user-id *users*)))
    (when (and user (or include-deleted (not (user-deleted user))))
      user)))

(defun find-by-username (username)
  (find-if (lambda (user)
             (and (string= (user-username user) username) (not (user-deleted user))))
           (all-users)))

(defun insert-user (username password role quota)
  (let ((user (make-user :id *next-user-id* :username username :password password
                         :role role :quota quota)))
    (setf (gethash (user-id user) *users*) user)
    (incf *next-user-id*)
    user))

(defun find-session (token)
  (gethash token *sessions*))

(defun insert-session (token user-id)
  (setf (gethash token *sessions*) (make-session :token token :user-id user-id)))

(defun remove-session (token)
  (remhash token *sessions*))

(defun find-project (project-id &optional include-deleted)
  (let ((project (gethash project-id *projects*)))
    (when (and project (or include-deleted (not (project-deleted project))))
      project)))

(defun insert-project (name owner-id)
  (let ((project (make-project :id *next-project-id* :name name :owner-id owner-id)))
    (setf (gethash (project-id project) *projects*) project)
    (incf *next-project-id*)
    project))

(defun find-task (task-id &optional include-deleted)
  (let ((task (gethash task-id *tasks*)))
    (when (and task (or include-deleted (not (task-deleted task))))
      task)))

(defun insert-task (project-id title priority assignee-id internal-note)
  (let ((task (make-task :id *next-task-id* :project-id project-id :title title
                         :priority priority :status "todo" :assignee-id assignee-id
                         :internal-note internal-note)))
    (setf (gethash (task-id task) *tasks*) task)
    (incf *next-task-id*)
    task))

(defun find-comment (comment-id)
  (gethash comment-id *comments*))

(defun insert-comment (task-id author-id body)
  (let ((comment (make-comment :id *next-comment-id* :task-id task-id
                               :author-id author-id :body body)))
    (setf (gethash (comment-id comment) *comments*) comment)
    (incf *next-comment-id*)
    comment))

(defun delete-comment (comment-id)
  (remhash comment-id *comments*))

(defun live-tasks-of (project-id)
  (remove-if-not (lambda (task)
                   (and (= (task-project-id task) project-id) (not (task-deleted task))))
                 (all-tasks)))

(defun task-count (project-id)
  (length (live-tasks-of project-id)))

(defun outbox-pending ()
  (count-if-not #'outbox-event-delivered *outbox*))

(defun find-idempotent (token key)
  "Return the recorded (status . body) pair for this token and key, or NIL."
  (gethash (list token key) *idempotency*))

(defun record-idempotent (token key status body)
  (setf (gethash (list token key) *idempotency*) (cons status body)))

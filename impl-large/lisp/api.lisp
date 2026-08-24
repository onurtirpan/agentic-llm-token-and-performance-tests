;;;; Task Service, large tier — HTTP routing, middleware and the entry point.

(defpackage :task-api
  (:use :cl :task-domain)
  (:local-nicknames (:store :task-store) (:service :task-service))
  ;; SEARCH names a Common Lisp function, so the endpoint of that name shadows it.
  (:shadow #:search))

(in-package :task-api)

;; Hunchentoot serves each request on its own thread, so the per-request state
;; that the middleware collects lives in specials that the chain rebinds.
(defvar *request-id* nil)
(defvar *user-id* nil)
(defvar *quota-remaining* nil)
(defvar *replayed* nil)
(defvar *route-label* "unmatched")

(defparameter +id-alphabet+ "0123456789abcdef")

;;; -------------------------------------------------------------------- helpers

(defun generate-id (size)
  "An opaque identifier. None of the three dependencies makes a UUID."
  (map-into (make-string size) (lambda () (char +id-alphabet+ (random 16)))))

(defun respond (status body)
  "Set the status and the content type, then hand jzon's text to Hunchentoot."
  (setf (hunchentoot:return-code*) status
        (hunchentoot:content-type*) "application/json")
  (com.inuoe.jzon:stringify body))

(defun no-content ()
  (setf (hunchentoot:return-code*) 204)
  "")

(defun envelope (error)
  (object "error"
          (object "code" (app-error-code error)
                  "message" (app-error-message error)
                  "requestId" *request-id*
                  "details" (coerce (loop for (field . message) in (app-error-details error)
                                          collect (object "field" field "message" message))
                                    'vector))))

(defun tagged (body version)
  (setf (hunchentoot:header-out :etag) (princ-to-string version))
  (respond 200 body))

(defun responded (status body)
  "A single-resource body carries its version, so the ETag comes for free."
  (let ((version (gethash "version" body)))
    (when version
      (setf (hunchentoot:header-out :etag) (princ-to-string version)))
    (respond status body)))

(defun begin (&optional admin)
  "Authenticate, charge the quota, then check the role. This order is fixed."
  (multiple-value-bind (user session)
      (service:authenticate (or (hunchentoot:header-in* :authorization) ""))
    (setf *user-id* (user-id user))
    (setf *quota-remaining* (service:charge-quota user session))
    (when admin
      (service:require-admin user))
    (values user session)))

(defun body-of ()
  (let ((raw (hunchentoot:raw-post-data :force-text t)))
    (if (or (null raw)
            (string= (string-trim '(#\Space #\Tab #\Newline #\Return) raw) ""))
        (object)
        (let ((parsed (handler-case (com.inuoe.jzon:parse raw)
                        (com.inuoe.jzon:json-parse-error () (bad-request)))))
          (unless (hash-table-p parsed)
            (bad-request))
          parsed))))

(defun whole (body field default)
  "Read an integer field. A JSON null reads as NIL, exactly as an absent one."
  (multiple-value-bind (value present) (gethash field body)
    (cond ((not present) default)
          ((eq value 'null) nil)
          ((integerp value) value)
          (t (bad-request)))))

(defun text (body field &optional (default ""))
  (multiple-value-bind (value present) (gethash field body)
    (cond ((not present) default)
          ((stringp value) value)
          (t (bad-request)))))

(defun parse-id (raw)
  (multiple-value-bind (value end) (parse-integer raw :junk-allowed t)
    (if (and value (= end (length raw)))
        value
        (bad-request))))

(defun integer-parameter (raw)
  "Return the integer a query parameter holds, or NIL when it holds anything else."
  (multiple-value-bind (value end) (parse-integer raw :junk-allowed t)
    (and value (= end (length raw)) value)))

(defun read-page (allowed)
  (let ((limit +default-limit+)
        (offset 0)
        (raw-limit (hunchentoot:get-parameter "limit"))
        (raw-offset (hunchentoot:get-parameter "offset"))
        (sort (or (hunchentoot:get-parameter "sort") (first allowed)))
        (order (or (hunchentoot:get-parameter "order") "asc"))
        (errors '()))
    (when raw-limit
      (setf limit (or (integer-parameter raw-limit) -1))
      (when (or (< limit 1) (> limit +max-limit+))
        (push (fail "limit" "limit is out of range") errors)))
    (when raw-offset
      (setf offset (or (integer-parameter raw-offset) -1))
      (when (< offset 0)
        (push (fail "offset" "offset is out of range") errors)))
    (unless (member sort allowed :test #'string=)
      (push (fail "sort" "sort is not a valid field") errors))
    (unless (member order '("asc" "desc") :test #'string=)
      (push (fail "order" "order must be asc or desc") errors))
    (when errors
      (invalid errors))
    (values limit offset sort order)))

(defun if-match (version)
  (service:check-if-match (hunchentoot:header-in* :if-match) version))

(defun idempotent (session produce)
  "Run PRODUCE once per Idempotency-Key, then replay the recorded outcome."
  (let ((key (hunchentoot:header-in* :idempotency-key)))
    (cond
      ((null key)
       (multiple-value-bind (status body) (funcall produce)
         (responded status body)))
      ((store:find-idempotent (session-token session) key)
       (setf *replayed* t)
       (let ((recorded (store:find-idempotent (session-token session) key)))
         (responded (car recorded) (cdr recorded))))
      (t
       (multiple-value-bind (status body)
           (handler-case (funcall produce)
             (app-error (error)
               (store:record-idempotent (session-token session) key
                                        (app-error-status error) (envelope error))
               (error error)))
         (store:record-idempotent (session-token session) key status body)
         (responded status body))))))

;;; ------------------------------------------------------------- health and auth

(defun get-health ()
  (respond 200 (object "status" "ok"
                       "projects" (count-if-not #'project-deleted (store:all-projects))
                       "tasks" (count-if-not #'task-deleted (store:all-tasks))
                       "comments" (hash-table-count store:*comments*))))

(defun login ()
  (let* ((body (body-of))
         (username (text body "username"))
         (password (text body "password"))
         (errors (remove nil (list (when (string= username "")
                                     (fail "username" "username is required"))
                                   (when (string= password "")
                                     (fail "password" "password is required"))))))
    (when errors
      (invalid errors))
    (let* ((token (generate-id 32))
           (user (service:login username password token)))
      (respond 200 (object "token" token "userId" (user-id user)
                           "role" (user-role user))))))

(defun logout ()
  (multiple-value-bind (user session) (begin)
    (declare (ignore user))
    (store:remove-session (session-token session))
    (no-content)))

(defun get-me ()
  (let ((user (begin)))
    (respond 200 (object "userId" (user-id user) "username" (user-username user)
                         "role" (user-role user)))))

;;; ---------------------------------------------------------------------- users

(defun list-users ()
  (begin t)
  (multiple-value-bind (limit offset sort order) (read-page +user-sorts+)
    (respond 200 (service:paginate
                  (mapcar #'service:serialize-user
                          (remove-if #'user-deleted (store:all-users)))
                  limit offset sort order))))

(defun create-user ()
  (multiple-value-bind (actor session) (begin t)
    (let ((body (body-of)))
      (idempotent session
                  (lambda ()
                    (let ((user (service:create-user actor
                                                     (text body "username")
                                                     (text body "password")
                                                     (gethash "role" body "user")
                                                     (gethash "quota" body +default-quota+))))
                      (values 201 (service:serialize-user user))))))))

(defun get-user (raw-id)
  (begin t)
  (let ((user (store:find-user (parse-id raw-id))))
    (when (null user)
      (not-found))
    (tagged (service:serialize-user user) (user-version user))))

(defun update-user (raw-id)
  (multiple-value-bind (actor session) (begin t)
    (declare (ignore session))
    (let ((user (store:find-user (parse-id raw-id))))
      (when (null user)
        (not-found))
      (if-match (user-version user))
      (service:update-user actor user (body-of))
      (tagged (service:serialize-user user) (user-version user)))))

(defun delete-user (raw-id)
  (multiple-value-bind (actor session) (begin t)
    (declare (ignore session))
    (let ((user (store:find-user (parse-id raw-id))))
      (when (null user)
        (not-found))
      (if-match (user-version user))
      (service:delete-user actor user)
      (tagged (service:serialize-user user) (user-version user)))))

;;; ------------------------------------------------------------------- projects

(defun list-projects ()
  (let* ((user (begin))
         (include (service:check-include-deleted
                   (hunchentoot:get-parameter "includeDeleted") user)))
    (multiple-value-bind (limit offset sort order) (read-page +project-sorts+)
      (respond 200 (service:paginate
                    (mapcar #'service:serialize-project
                            (service:visible-projects user include))
                    limit offset sort order)))))

(defun create-project ()
  (multiple-value-bind (actor session) (begin t)
    (let ((body (body-of)))
      (idempotent session
                  (lambda ()
                    (let ((project (service:create-project
                                    actor (text body "name")
                                    (whole body "ownerId" (user-id actor)))))
                      (values 201 (service:serialize-project project))))))))

(defun get-project (raw-id)
  (let* ((user (begin))
         (project (service:reachable-project (parse-id raw-id) user)))
    (tagged (service:serialize-project project) (project-version project))))

(defun update-project (raw-id)
  (multiple-value-bind (actor session) (begin t)
    (declare (ignore session))
    (let ((project (service:reachable-project (parse-id raw-id) actor)))
      (if-match (project-version project))
      (let ((body (body-of)))
        (when (has-field body "name")
          (service:rename-project actor project (text body "name"))))
      (tagged (service:serialize-project project) (project-version project)))))

(defun delete-project (raw-id)
  (multiple-value-bind (actor session) (begin t)
    (declare (ignore session))
    (let ((project (service:reachable-project (parse-id raw-id) actor)))
      (if-match (project-version project))
      (service:delete-project actor project)
      (tagged (service:serialize-project project) (project-version project)))))

(defun restore-project (raw-id)
  (multiple-value-bind (actor session) (begin t)
    (declare (ignore session))
    (let ((project (service:reachable-project (parse-id raw-id) actor t)))
      (if-match (project-version project))
      (service:restore-project actor project)
      (tagged (service:serialize-project project) (project-version project)))))

;;; ---------------------------------------------------------------------- tasks

(defun task-filters (rows)
  (let* ((status (hunchentoot:get-parameter "status"))
         (assignee (hunchentoot:get-parameter "assigneeId"))
         (errors (remove nil (list (when (and status
                                              (not (member status +statuses+ :test #'string=)))
                                     (fail "status" "status is not valid"))
                                   (when (and assignee (null (integer-parameter assignee)))
                                     (fail "assigneeId" "assigneeId is not a known user"))))))
    (when errors
      (invalid errors))
    (when status
      (setf rows (remove-if-not (lambda (task) (string= (task-status task) status)) rows)))
    (when assignee
      (setf rows (remove-if-not (lambda (task)
                                  (eql (task-assignee-id task)
                                       (integer-parameter assignee)))
                                rows)))
    rows))

(defun list-all-tasks ()
  (let* ((user (begin))
         (include (service:check-include-deleted
                   (hunchentoot:get-parameter "includeDeleted") user)))
    (multiple-value-bind (limit offset sort order) (read-page +task-sorts+)
      (let ((rows (task-filters (service:visible-tasks user include))))
        (respond 200 (service:paginate
                      (mapcar (lambda (task) (service:serialize-task task (user-role user)))
                              rows)
                      limit offset sort order))))))

(defun list-tasks (raw-id)
  (let* ((user (begin))
         (project (service:reachable-project (parse-id raw-id) user)))
    (multiple-value-bind (limit offset sort order) (read-page +task-sorts+)
      (let ((rows (store:live-tasks-of (project-id project))))
        (respond 200 (service:paginate
                      (mapcar (lambda (task) (service:serialize-task task (user-role user)))
                              rows)
                      limit offset sort order))))))

(defun create-task (raw-id)
  (multiple-value-bind (actor session) (begin)
    (let* ((project (service:reachable-project (parse-id raw-id) actor))
           (body (body-of)))
      (idempotent session
                  (lambda ()
                    (multiple-value-bind (note detail) (service:read-note actor body "")
                      (let ((task (service:create-task actor project
                                                       (text body "title")
                                                       (whole body "priority" 0)
                                                       (whole body "assigneeId" nil)
                                                       note (remove nil (list detail)))))
                        (values 201 (service:serialize-task task (user-role actor))))))))))

(defun get-task (raw-id)
  (let* ((user (begin))
         (task (service:reachable-task (parse-id raw-id) user)))
    (tagged (service:serialize-task task (user-role user)) (task-version task))))

(defun replace-task (raw-id)
  (multiple-value-bind (actor session) (begin)
    (declare (ignore session))
    (let ((task (service:reachable-task (parse-id raw-id) actor)))
      (if-match (task-version task))
      (let ((body (body-of)))
        (multiple-value-bind (note detail)
            (service:read-note actor body (task-internal-note task))
          (service:replace-task actor task (text body "title")
                                (whole body "priority" 0)
                                (whole body "assigneeId" nil)
                                note (remove nil (list detail)))))
      (tagged (service:serialize-task task (user-role actor)) (task-version task)))))

(defun update-status (raw-id)
  (multiple-value-bind (actor session) (begin)
    (declare (ignore session))
    (let ((task (service:reachable-task (parse-id raw-id) actor)))
      (if-match (task-version task))
      (service:move-status actor task (gethash "status" (body-of)))
      (tagged (service:serialize-task task (user-role actor)) (task-version task)))))

(defun delete-task (raw-id)
  (multiple-value-bind (actor session) (begin)
    (declare (ignore session))
    (let ((task (service:reachable-task (parse-id raw-id) actor)))
      (if-match (task-version task))
      (service:delete-task actor task)
      (tagged (service:serialize-task task (user-role actor)) (task-version task)))))

(defun restore-task (raw-id)
  (multiple-value-bind (actor session) (begin)
    (declare (ignore session))
    (let ((task (service:reachable-task (parse-id raw-id) actor t)))
      (if-match (task-version task))
      (service:restore-task actor task)
      (tagged (service:serialize-task task (user-role actor)) (task-version task)))))

(defun apply-bulk (actor item)
  "Answer one bulk entry with the status and the resource id it produced."
  (let ((operation (gethash "op" item)))
    (cond
      ((equal operation "create")
       (let* ((project (service:reachable-project (whole item "projectId" 0) actor))
              (task (service:create-task actor project (text item "title")
                                         (whole item "priority" 0) nil "" '())))
         (values 201 (task-id task))))
      ((equal operation "status")
       (let ((task (service:reachable-task (whole item "id" 0) actor)))
         (service:check-if-match (princ-to-string (gethash "version" item))
                                 (task-version task))
         (service:move-status actor task (gethash "status" item))
         (values 200 (task-id task))))
      ((equal operation "delete")
       (let ((task (service:reachable-task (whole item "id" 0) actor)))
         (service:check-if-match (princ-to-string (gethash "version" item))
                                 (task-version task))
         (service:delete-task actor task)
         (values 200 (task-id task))))
      (t (invalid (list (fail "op" "op is not valid")))))))

(defun bulk-tasks ()
  (multiple-value-bind (actor session) (begin)
    (declare (ignore session))
    (let ((operations (gethash "operations" (body-of)))
          (results '()))
      (service:check-bulk-size operations)
      (dotimes (index (length operations))
        (let ((item (aref operations index)))
          (push (handler-case
                    (multiple-value-bind (status id)
                        (progn (unless (hash-table-p item) (bad-request))
                               (apply-bulk actor item))
                      (object "index" index "status" status "id" id "error" 'null))
                  (app-error (error)
                    (object "index" index "status" (app-error-status error)
                            "id" 'null "error" (app-error-code error))))
                results)))
      (respond 200 (object "results" (coerce (nreverse results) 'vector))))))

;;; ------------------------------------------------------------------- comments

(defun list-comments (raw-id)
  (let* ((user (begin))
         (task (service:reachable-task (parse-id raw-id) user)))
    (multiple-value-bind (limit offset sort order) (read-page +comment-sorts+)
      (respond 200 (service:paginate
                    (mapcar #'service:serialize-comment
                            (remove-if-not (lambda (comment)
                                             (= (comment-task-id comment) (task-id task)))
                                           (store:all-comments)))
                    limit offset sort order)))))

(defun create-comment (raw-id)
  (multiple-value-bind (actor session) (begin)
    (let* ((task (service:reachable-task (parse-id raw-id) actor))
           (body (body-of)))
      (idempotent session
                  (lambda ()
                    (let ((comment (service:create-comment actor task (text body "body"))))
                      (values 201 (service:serialize-comment comment))))))))

(defun delete-comment (raw-id)
  (multiple-value-bind (actor session) (begin)
    (declare (ignore session))
    (let ((comment (store:find-comment (parse-id raw-id))))
      (when (null comment)
        (not-found))
      (service:reachable-task (comment-task-id comment) actor t)
      (service:remove-comment actor comment)
      (no-content))))

;;; ------------------------------------------------ search, reports, telemetry

(defun search ()
  (let* ((user (begin))
         (query (or (hunchentoot:get-parameter "q") "")))
    (when (string= query "")
      (invalid (list (fail "q" "q is required"))))
    (respond 200 (service:search user query))))

(defun workload ()
  (let* ((user (begin))
         (group-by (or (hunchentoot:get-parameter "groupBy") "status")))
    (unless (member group-by +group-bys+ :test #'string=)
      (invalid (list (fail "groupBy" "groupBy is not valid"))))
    (respond 200 (service:workload user group-by))))

(defun list-audit ()
  (begin t)
  (multiple-value-bind (limit offset sort order) (read-page +seq-sorts+)
    (let ((actor-id (hunchentoot:get-parameter "actorId"))
          (resource (hunchentoot:get-parameter "resource"))
          (action (hunchentoot:get-parameter "action")))
      (respond 200 (service:paginate
                    (mapcar #'service:serialize-audit
                            (remove-if-not
                             (lambda (entry)
                               (and (or (null actor-id)
                                        (string= (princ-to-string
                                                  (audit-entry-actor-id entry))
                                                 actor-id))
                                    (or (null resource)
                                        (string= (audit-entry-resource entry) resource))
                                    (or (null action)
                                        (string= (audit-entry-action entry) action))))
                             (store:audit-entries)))
                    limit offset sort order)))))

(defun list-outbox ()
  (begin t)
  (multiple-value-bind (limit offset sort order) (read-page +seq-sorts+)
    (let ((wanted (hunchentoot:get-parameter "delivered")))
      (respond 200 (service:paginate
                    (mapcar #'service:serialize-outbox
                            (remove-if-not
                             (lambda (event)
                               (or (null wanted)
                                   (eq (and (outbox-event-delivered event) t)
                                       (string= wanted "true"))))
                             (store:outbox-events)))
                    limit offset sort order)))))

(defun flush-outbox ()
  (begin t)
  (respond 200 (object "flushed" (service:flush-outbox))))

(defun get-metrics ()
  (begin t)
  (respond 200 (service:metrics)))

(defun get-stats ()
  (begin t)
  (respond 200 (service:stats)))

(defun fallback ()
  (not-found))

;;; --------------------------------------------------------------------- routing

(defparameter *routes*
  (list (list :get "^/health$" "GET /health" #'get-health)
        (list :post "^/auth/login$" "POST /auth/login" #'login)
        (list :post "^/auth/logout$" "POST /auth/logout" #'logout)
        (list :get "^/me$" "GET /me" #'get-me)
        (list :get "^/users$" "GET /users" #'list-users)
        (list :post "^/users$" "POST /users" #'create-user)
        (list :get "^/users/([^/]+)$" "GET /users/{id}" #'get-user)
        (list :patch "^/users/([^/]+)$" "PATCH /users/{id}" #'update-user)
        (list :delete "^/users/([^/]+)$" "DELETE /users/{id}" #'delete-user)
        (list :get "^/projects$" "GET /projects" #'list-projects)
        (list :post "^/projects$" "POST /projects" #'create-project)
        (list :get "^/projects/([^/]+)$" "GET /projects/{id}" #'get-project)
        (list :patch "^/projects/([^/]+)$" "PATCH /projects/{id}" #'update-project)
        (list :delete "^/projects/([^/]+)$" "DELETE /projects/{id}" #'delete-project)
        (list :post "^/projects/([^/]+)/restore$" "POST /projects/{id}/restore"
              #'restore-project)
        (list :get "^/projects/([^/]+)/tasks$" "GET /projects/{id}/tasks" #'list-tasks)
        (list :post "^/projects/([^/]+)/tasks$" "POST /projects/{id}/tasks" #'create-task)
        (list :get "^/tasks$" "GET /tasks" #'list-all-tasks)
        (list :post "^/tasks/bulk$" "POST /tasks/bulk" #'bulk-tasks)
        (list :get "^/tasks/([^/]+)$" "GET /tasks/{id}" #'get-task)
        (list :put "^/tasks/([^/]+)$" "PUT /tasks/{id}" #'replace-task)
        (list :delete "^/tasks/([^/]+)$" "DELETE /tasks/{id}" #'delete-task)
        (list :patch "^/tasks/([^/]+)/status$" "PATCH /tasks/{id}/status" #'update-status)
        (list :post "^/tasks/([^/]+)/restore$" "POST /tasks/{id}/restore" #'restore-task)
        (list :get "^/tasks/([^/]+)/comments$" "GET /tasks/{id}/comments" #'list-comments)
        (list :post "^/tasks/([^/]+)/comments$" "POST /tasks/{id}/comments" #'create-comment)
        (list :delete "^/comments/([^/]+)$" "DELETE /comments/{id}" #'delete-comment)
        (list :get "^/search$" "GET /search" #'search)
        (list :get "^/reports/workload$" "GET /reports/workload" #'workload)
        (list :get "^/audit$" "GET /audit" #'list-audit)
        (list :get "^/outbox$" "GET /outbox" #'list-outbox)
        (list :post "^/outbox/flush$" "POST /outbox/flush" #'flush-outbox)
        (list :get "^/metrics$" "GET /metrics" #'get-metrics)
        (list :get "^/stats$" "GET /stats" #'get-stats)))

(defun route ()
  "Match the method and the path, and remember the pattern that metrics reports."
  (let ((method (hunchentoot:request-method*))
        (path (hunchentoot:script-name*)))
    (loop for (verb pattern label handler) in *routes*
          do (when (eq verb method)
               (multiple-value-bind (match groups) (cl-ppcre:scan-to-strings pattern path)
                 (when match
                   (setf *route-label* label)
                   (return-from route (apply handler (coerce groups 'list)))))))
    (fallback)))

;;; ----------------------------------------------------------------- middleware

(defun write-log-line (status started audit-seq)
  (write-string
   (com.inuoe.jzon:stringify
    (object "level" (cond ((>= status 500) "error")
                          ((>= status 400) "warn")
                          (t "info"))
            "requestId" *request-id*
            "method" (symbol-name (hunchentoot:request-method*))
            "path" (hunchentoot:script-name*)
            "status" status
            "durationMs" (round (* 1000 (- (get-internal-real-time) started))
                                internal-time-units-per-second)
            "userId" (or *user-id* 'null)
            "quotaRemaining" (or *quota-remaining* 'null)
            "auditSeq" audit-seq)))
  (terpri)
  (finish-output))

(defun handle-request ()
  "The chain: request id, route match, handler, response headers, then the log."
  (bordeaux-threads:with-lock-held (store:*state-lock*)
    (let* ((given (hunchentoot:header-in* :x-request-id))
           (*request-id* (if (and given (string/= given "")) given (generate-id 12)))
           (*user-id* nil)
           (*quota-remaining* nil)
           (*replayed* nil)
           (*route-label* "unmatched")
           (before (length store:*audit*))
           (started (get-internal-real-time))
           (payload (handler-case (route)
                      (app-error (error)
                        (respond (app-error-status error) (envelope error)))))
           (status (hunchentoot:return-code*)))
      (setf (hunchentoot:header-out :x-request-id) *request-id*)
      (when *quota-remaining*
        (setf (hunchentoot:header-out :x-quota-remaining)
              (princ-to-string *quota-remaining*)))
      (when *replayed*
        (setf (hunchentoot:header-out :idempotency-replayed) "true"))
      (store:count-request *route-label* status)
      (write-log-line status started (- (length store:*audit*) before))
      payload)))

;;; --------------------------------------------------------------- entry point

(defvar *server*
  (make-instance 'hunchentoot:easy-acceptor
                 :port +port+
                 :address "127.0.0.1"
                 :access-log-destination nil
                 :message-log-destination nil))

(setf hunchentoot:*dispatch-table*
      (list (lambda (request) (declare (ignore request)) #'handle-request)))
(setf hunchentoot:*show-lisp-errors-p* nil)
(setf *random-state* (make-random-state t))

(store:seed)
(hunchentoot:start *server*)
(loop (sleep 3600))

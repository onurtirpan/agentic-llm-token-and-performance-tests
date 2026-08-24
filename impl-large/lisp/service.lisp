;;;; Task Service, large tier — business rules, authorization and audit emission.

(defpackage :task-service
  (:use :cl :task-domain)
  (:local-nicknames (:store :task-store))
  ;; SEARCH names a Common Lisp function, so the endpoint of that name shadows it.
  (:shadow #:search)
  (:export #:serialize-user #:serialize-project #:serialize-task #:serialize-comment
           #:serialize-audit #:serialize-outbox
           #:authenticate #:charge-quota #:require-admin #:reachable-project
           #:reachable-task #:check-if-match #:check-include-deleted #:paginate
           #:login #:create-project #:rename-project #:delete-project #:restore-project
           #:read-note #:create-task #:replace-task #:move-status #:delete-task
           #:restore-task #:create-comment #:remove-comment
           #:create-user #:update-user #:delete-user
           #:visible-projects #:visible-tasks #:search #:workload #:flush-outbox
           #:metrics #:stats #:check-bulk-size))

(in-package :task-service)

;;; ---------------------------------------------------------------- serializers

(defun serialize-user (user)
  (object "id" (user-id user) "username" (user-username user) "role" (user-role user)
          "quota" (user-quota user) "version" (user-version user)
          "deleted" (user-deleted user)))

(defun serialize-project (project)
  (object "id" (project-id project) "name" (project-name project)
          "ownerId" (project-owner-id project)
          "taskCount" (store:task-count (project-id project))
          "version" (project-version project) "deleted" (project-deleted project)))

(defun serialize-task (task role)
  (let ((body (object "id" (task-id task) "projectId" (task-project-id task)
                      "title" (task-title task) "priority" (task-priority task)
                      "status" (task-status task)
                      "assigneeId" (or (task-assignee-id task) 'null))))
    (when (string= role "admin")
      (setf (gethash "internalNote" body) (task-internal-note task)))
    (setf (gethash "version" body) (task-version task)
          (gethash "deleted" body) (task-deleted task)
          (gethash "score" body) (compute-score (task-priority task) (task-status task)))
    body))

(defun serialize-comment (comment)
  (object "id" (comment-id comment) "taskId" (comment-task-id comment)
          "authorId" (comment-author-id comment) "body" (comment-body comment)))

(defun serialize-audit (entry)
  (object "seq" (audit-entry-seq entry) "actorId" (audit-entry-actor-id entry)
          "action" (audit-entry-action entry) "resource" (audit-entry-resource entry)
          "resourceId" (audit-entry-resource-id entry)))

(defun serialize-outbox (event)
  (object "seq" (outbox-event-seq event) "name" (outbox-event-name event)
          "resourceId" (outbox-event-resource-id event)
          "delivered" (outbox-event-delivered event)))

;;; --------------------------------------------------------------- access rules

(defun authenticate (header)
  (let* ((token (if (and (>= (length header) 7) (string= "Bearer " header :end2 7))
                    (subseq header 7)
                    ""))
         (session (store:find-session token)))
    (when (null session)
      (unauthorized))
    (let ((user (store:find-user (session-user-id session))))
      (when (null user)
        (unauthorized))
      (values user session))))

(defun charge-quota (user session)
  (when (>= (session-used session) (user-quota user))
    (quota-exceeded))
  (incf (session-used session))
  (max (- (user-quota user) (session-used session)) 0))

(defun require-admin (user)
  (unless (string= (user-role user) "admin")
    (forbidden)))

(defun reachable-project (project-id user &optional include-deleted)
  (let ((project (store:find-project project-id include-deleted)))
    (when (null project)
      (not-found))
    (when (and (string/= (user-role user) "admin")
               (/= (project-owner-id project) (user-id user)))
      (forbidden))
    project))

(defun reachable-task (task-id user &optional include-deleted)
  (let ((task (store:find-task task-id include-deleted)))
    (when (null task)
      (not-found))
    (reachable-project (task-project-id task) user t)
    task))

(defun check-if-match (header version)
  (when (or (null header) (string= header ""))
    (precondition-required))
  (unless (string= header (princ-to-string version))
    (precondition-failed)))

(defun check-include-deleted (raw user)
  (cond ((null raw) nil)
        ((string/= (user-role user) "admin") (forbidden))
        (t (string= raw "true"))))

;;; ----------------------------------------------------------------- pagination

(defun row< (left right)
  "Order two sort keys, which are either both numbers or both strings."
  (if (stringp left)
      (and (string< left right) t)
      (< left right)))

(defun paginate (rows limit offset sort order)
  "Sort by the tiebreak first, then stably by the requested field."
  (let* ((tiebreak (if (and rows (has-field (first rows) "seq")) "seq" "id"))
         (ranked (stable-sort rows #'row< :key (lambda (row) (gethash tiebreak row))))
         (sorted (stable-sort ranked
                              (if (string= order "desc")
                                  (lambda (left right) (row< right left))
                                  #'row<)
                              :key (lambda (row) (gethash sort row))))
         (total (length sorted))
         (start (min offset total))
         (end (min (+ offset limit) total)))
    (object "items" (coerce (subseq sorted start end) 'vector)
            "total" total "limit" limit "offset" offset)))

;;; ----------------------------------------------------------------------- auth

(defun login (username password token)
  (let ((user (store:find-by-username username)))
    (when (or (null user) (string/= (user-password user) password))
      (invalid-credentials))
    (store:insert-session token (user-id user))
    user))

;;; ------------------------------------------------------------------- projects

(defun name-taken-p (name owner-id except-id)
  (find-if (lambda (project)
             (and (eql (project-owner-id project) owner-id)
                  (string= (project-name project) name)
                  (not (eql (project-id project) except-id))
                  (not (project-deleted project))))
           (store:all-projects)))

(defun create-project (actor name owner-id)
  (let ((errors (remove nil (list (check-string name "name" +max-name-length+)
                                  (unless (store:find-user owner-id)
                                    (fail "ownerId" "ownerId is not a known user"))))))
    (when errors
      (invalid errors))
    (when (name-taken-p name owner-id nil)
      (conflict))
    (let ((project (store:insert-project name owner-id)))
      (store:record (user-id actor) "create" "project" (project-id project))
      project)))

(defun rename-project (actor project name)
  (let ((errors (remove nil (list (check-string name "name" +max-name-length+)))))
    (when errors
      (invalid errors))
    (when (name-taken-p name (project-owner-id project) (project-id project))
      (conflict))
    (setf (project-name project) name)
    (incf (project-version project))
    (store:record (user-id actor) "update" "project" (project-id project))
    project))

(defun delete-project (actor project)
  (setf (project-deleted project) t)
  (incf (project-version project))
  (store:record (user-id actor) "delete" "project" (project-id project))
  (dolist (task (store:live-tasks-of (project-id project)))
    (setf (task-deleted task) t)
    (incf (task-version task))
    (store:record (user-id actor) "delete" "task" (task-id task)))
  project)

(defun restore-project (actor project)
  (unless (project-deleted project)
    (conflict))
  (setf (project-deleted project) nil)
  (incf (project-version project))
  (store:record (user-id actor) "restore" "project" (project-id project))
  project)

;;; ---------------------------------------------------------------------- tasks

(defun read-note (actor body current)
  "Return the note to store, plus the detail when it is too long."
  (multiple-value-bind (note present) (gethash "internalNote" body)
    (cond ((not present) (values current nil))
          ((string/= (user-role actor) "admin") (forbidden))
          ((not (stringp note)) (bad-request))
          ((> (length note) +max-title-length+)
           (values note (fail "internalNote" "internalNote is too long")))
          (t (values note nil)))))

(defun check-assignee (assignee-id)
  (when (and assignee-id (null (store:find-user assignee-id)))
    (fail "assigneeId" "assigneeId is not a known user")))

(defun create-task (actor project title priority assignee-id note extra-errors)
  (let ((errors (append extra-errors
                        (remove nil (list (check-string title "title" +max-title-length+)
                                          (check-priority priority)
                                          (check-assignee assignee-id))))))
    (when errors
      (invalid errors))
    (let ((task (store:insert-task (project-id project) title priority assignee-id note)))
      (store:record (user-id actor) "create" "task" (task-id task))
      task)))

(defun replace-task (actor task title priority assignee-id note extra-errors)
  (let ((errors (append extra-errors
                        (remove nil (list (check-string title "title" +max-title-length+)
                                          (check-priority priority)
                                          (check-assignee assignee-id))))))
    (when errors
      (invalid errors))
    (setf (task-title task) title
          (task-priority task) priority
          (task-assignee-id task) assignee-id
          (task-internal-note task) note)
    (incf (task-version task))
    (store:record (user-id actor) "update" "task" (task-id task))
    task))

(defun move-status (actor task status)
  (let ((errors (remove nil (list (check-status status)))))
    (when errors
      (invalid errors))
    (unless (member (cons (task-status task) status) +transitions+ :test #'equal)
      (invalid-transition))
    (setf (task-status task) status)
    (incf (task-version task))
    (store:record (user-id actor) "update" "task" (task-id task))
    task))

(defun delete-task (actor task)
  (setf (task-deleted task) t)
  (incf (task-version task))
  (store:record (user-id actor) "delete" "task" (task-id task))
  task)

(defun restore-task (actor task)
  (unless (task-deleted task)
    (conflict))
  (setf (task-deleted task) nil)
  (incf (task-version task))
  (store:record (user-id actor) "restore" "task" (task-id task))
  task)

;;; ------------------------------------------------------------------- comments

(defun create-comment (actor task body)
  (let ((errors (remove nil (list (check-string body "body" +max-comment-length+)))))
    (when errors
      (invalid errors))
    (let ((comment (store:insert-comment (task-id task) (user-id actor) body)))
      (store:record (user-id actor) "create" "comment" (comment-id comment))
      comment)))

(defun remove-comment (actor comment)
  (when (and (string/= (user-role actor) "admin")
             (/= (comment-author-id comment) (user-id actor)))
    (forbidden))
  (store:delete-comment (comment-id comment))
  (store:record (user-id actor) "delete" "comment" (comment-id comment)))

;;; ---------------------------------------------------------------------- users

(defun create-user (actor username password role quota)
  (let ((errors (remove nil (list (check-string username "username" +max-name-length+)
                                  (check-string password "password" +max-name-length+)
                                  (check-role role)
                                  (check-quota quota)))))
    (when errors
      (invalid errors))
    (when (store:find-by-username username)
      (conflict))
    (let ((user (store:insert-user username password role quota)))
      (store:record (user-id actor) "create" "user" (user-id user))
      user)))

(defun update-user (actor user body)
  (let ((errors (remove nil (list (when (has-field body "role")
                                    (check-role (gethash "role" body)))
                                  (when (has-field body "quota")
                                    (check-quota (gethash "quota" body)))))))
    (when errors
      (invalid errors))
    (when (has-field body "role")
      (setf (user-role user) (gethash "role" body)))
    (when (has-field body "quota")
      (setf (user-quota user) (gethash "quota" body)))
    (incf (user-version user))
    (store:record (user-id actor) "update" "user" (user-id user))
    user))

(defun delete-user (actor user)
  (when (= (user-id user) (user-id actor))
    (conflict))
  (setf (user-deleted user) t)
  (incf (user-version user))
  (store:record (user-id actor) "delete" "user" (user-id user))
  user)

;;; -------------------------------------------------------- queries and reports

(defun visible-projects (user include-deleted)
  (remove-if-not (lambda (project)
                   (and (or include-deleted (not (project-deleted project)))
                        (or (string= (user-role user) "admin")
                            (= (project-owner-id project) (user-id user)))))
                 (store:all-projects)))

(defun visible-tasks (user include-deleted)
  (let ((allowed (mapcar #'project-id (visible-projects user t))))
    (remove-if-not (lambda (task)
                     (and (member (task-project-id task) allowed)
                          (or include-deleted (not (task-deleted task)))))
                   (store:all-tasks))))

(defun search (user query)
  (let* ((needle (string-downcase query))
         (results
           (append
            (loop for project in (visible-projects user nil)
                  when (cl:search needle (string-downcase (project-name project)))
                    collect (object "type" "project" "id" (project-id project)
                                    "label" (project-name project)))
            (loop for task in (visible-tasks user nil)
                  when (cl:search needle (string-downcase (task-title task)))
                    collect (object "type" "task" "id" (task-id task)
                                    "label" (task-title task))))))
    (object "results" (coerce results 'vector) "total" (length results))))

(defun total-score (tasks)
  (reduce #'+ tasks :initial-value 0
                    :key (lambda (task)
                           (compute-score (task-priority task) (task-status task)))))

(defun group-of (key tasks)
  (object "key" key "tasks" (length tasks) "totalScore" (total-score tasks)))

(defun workload (user group-by)
  (let* ((rows (visible-tasks user nil))
         (groups
           (cond
             ((string= group-by "status")
              (loop for status in +statuses+
                    collect (group-of status
                                      (remove-if-not (lambda (task)
                                                       (string= (task-status task) status))
                                                     rows))))
             ((string= group-by "assignee")
              (let ((named (sort (remove-duplicates
                                  (remove nil (mapcar #'task-assignee-id rows)))
                                 #'<))
                    (loose (remove-if #'task-assignee-id rows)))
                (append (loop for assignee in named
                              collect (group-of (princ-to-string assignee)
                                                (remove-if-not
                                                 (lambda (task)
                                                   (eql (task-assignee-id task) assignee))
                                                 rows)))
                        (when loose
                          (list (group-of "unassigned" loose))))))
             (t
              (loop for project in (visible-projects user nil)
                    collect (group-of (project-name project)
                                      (remove-if-not
                                       (lambda (task)
                                         (= (task-project-id task) (project-id project)))
                                       rows)))))))
    (object "groupBy" group-by "groups" (coerce groups 'vector))))

(defun flush-outbox ()
  (let ((pending (remove-if #'outbox-event-delivered (store:outbox-events))))
    (dolist (event pending)
      (setf (outbox-event-delivered event) t))
    (length pending)))

(defun metrics ()
  (let ((by-status (object))
        (codes (sort (loop for code being the hash-keys of store:*by-status* collect code) #'<))
        (routes (sort (loop for route being the hash-keys of store:*by-route* collect route)
                      #'string<)))
    (dolist (code codes)
      (setf (gethash (princ-to-string code) by-status) (gethash code store:*by-status*)))
    (object "requests" store:*requests*
            "byStatus" by-status
            "byRoute" (coerce (loop for route in routes
                                    collect (object "route" route
                                                    "count" (gethash route store:*by-route*)))
                              'vector)
            "auditEntries" (length store:*audit*)
            "outboxPending" (store:outbox-pending))))

(defun stats ()
  (let* ((live (remove-if #'task-deleted (store:all-tasks)))
         (total (length live))
         (counts (object))
         (best nil))
    (dolist (status +statuses+)
      (setf (gethash status counts) 0))
    (dolist (task live)
      (incf (gethash (task-status task) counts)))
    (dolist (project (store:all-projects))
      (unless (project-deleted project)
        (when (or (null best)
                  (> (store:task-count (project-id project))
                     (store:task-count (project-id best))))
          (setf best project))))
    (object "projects" (count-if-not #'project-deleted (store:all-projects))
            "tasks" total
            "users" (count-if-not #'user-deleted (store:all-users))
            "sessions" (hash-table-count store:*sessions*)
            "comments" (hash-table-count store:*comments*)
            "byStatus" counts
            "avgScore" (if (zerop total)
                           0.0d0
                           (/ (fround (/ (* 100.0d0 (total-score live)) total)) 100))
            "topProjectName" (if best (project-name best) 'null)
            "auditEntries" (length store:*audit*)
            "outboxPending" (store:outbox-pending))))

(defun check-bulk-size (operations)
  (unless (and (simple-vector-p operations)
               (<= 1 (length operations) +max-bulk-items+))
    (invalid (list (fail "operations" "operations is out of range")))))

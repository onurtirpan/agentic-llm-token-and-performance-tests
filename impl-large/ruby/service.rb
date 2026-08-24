# frozen_string_literal: true

# Task Service, large tier — business rules, authorization and audit emission.

require_relative "domain"
require_relative "store"

module Service
  module_function

  # ---------------------------------------------------------------- serializers

  def serialize_user(user)
    { id: user.id, username: user.username, role: user.role,
      quota: user.quota, version: user.version, deleted: user.deleted }
  end

  def serialize_project(project)
    { id: project.id, name: project.name, ownerId: project.owner_id,
      taskCount: Store.task_count(project.id), version: project.version,
      deleted: project.deleted }
  end

  def serialize_task(task, role)
    body = { id: task.id, projectId: task.project_id, title: task.title,
             priority: task.priority, status: task.status, assigneeId: task.assignee_id }
    body[:internalNote] = task.internal_note if role == "admin"
    body[:version] = task.version
    body[:deleted] = task.deleted
    body[:score] = Domain.compute_score(task.priority, task.status)
    body
  end

  def serialize_comment(comment)
    { id: comment.id, taskId: comment.task_id,
      authorId: comment.author_id, body: comment.body }
  end

  def serialize_audit(entry)
    { seq: entry.seq, actorId: entry.actor_id, action: entry.action,
      resource: entry.resource, resourceId: entry.resource_id }
  end

  def serialize_outbox(event)
    { seq: event.seq, name: event.name, resourceId: event.resource_id,
      delivered: event.delivered }
  end

  # --------------------------------------------------------------- access rules

  def authenticate(header)
    token = header.start_with?("Bearer ") ? header.delete_prefix("Bearer ") : ""
    session = Store.sessions[token]
    raise Domain.unauthorized if session.nil?

    user = Store.find_user(session.user_id)
    raise Domain.unauthorized if user.nil?

    [user, session]
  end

  def charge_quota(user, session)
    raise Domain.quota_exceeded if session.used >= user.quota

    session.used += 1
    [user.quota - session.used, 0].max
  end

  def require_admin(user)
    raise Domain.forbidden unless user.role == "admin"
  end

  def reachable_project(project_id, user, include_deleted = false)
    project = Store.find_project(project_id, include_deleted)
    raise Domain.not_found if project.nil?
    raise Domain.forbidden if user.role != "admin" && project.owner_id != user.id

    project
  end

  def reachable_task(task_id, user, include_deleted = false)
    task = Store.find_task(task_id, include_deleted)
    raise Domain.not_found if task.nil?

    reachable_project(task.project_id, user, true)
    task
  end

  def check_if_match(header, version)
    raise Domain.precondition_required if header.nil? || header.empty?
    raise Domain.precondition_failed if header != version.to_s
  end

  def check_include_deleted(raw, user)
    return false if raw.nil?
    raise Domain.forbidden if user.role != "admin"

    raw == "true"
  end

  # ----------------------------------------------------------------- pagination

  # Ruby does not sort stably, so the tiebreak is spelled out in the comparator.
  # It stays ascending even when the requested order is descending.
  def paginate(rows, limit, offset, sort, order)
    key = sort.to_sym
    tiebreak = rows.first&.key?(:seq) ? :seq : :id
    sorted = rows.sort do |left, right|
      ranking = left[key] <=> right[key]
      ranking = -ranking if order == "desc"
      ranking.zero? ? left[tiebreak] <=> right[tiebreak] : ranking
    end
    { items: sorted[offset, limit] || [], total: sorted.length,
      limit: limit, offset: offset }
  end

  # ------------------------------------------------------------------------ auth

  def login(username, password, token)
    user = Store.find_by_username(username)
    raise Domain.invalid_credentials if user.nil? || user.password != password

    Store.sessions[token] = Domain::Session.new(token, user.id, 0)
    user
  end

  # --------------------------------------------------------------------- projects

  def create_project(actor, name, owner_id)
    errors = []
    Domain.check_string(name, "name", Domain::MAX_NAME_LENGTH, errors)
    if Store.find_user(owner_id).nil?
      errors << Domain.fail_with("ownerId", "ownerId is not a known user")
    end
    raise Domain.invalid(errors) unless errors.empty?

    duplicate = Store.projects.values.any? do |other|
      other.owner_id == owner_id && other.name == name && !other.deleted
    end
    raise Domain.conflict if duplicate

    project = Store.insert_project(name, owner_id)
    Store.record(actor.id, "create", "project", project.id)
    project
  end

  def rename_project(actor, project, name)
    errors = []
    Domain.check_string(name, "name", Domain::MAX_NAME_LENGTH, errors)
    raise Domain.invalid(errors) unless errors.empty?

    duplicate = Store.projects.values.any? do |other|
      other.owner_id == project.owner_id && other.name == name &&
        other.id != project.id && !other.deleted
    end
    raise Domain.conflict if duplicate

    project.name = name
    project.version += 1
    Store.record(actor.id, "update", "project", project.id)
    project
  end

  def delete_project(actor, project)
    project.deleted = true
    project.version += 1
    Store.record(actor.id, "delete", "project", project.id)
    Store.live_tasks_of(project.id).each do |task|
      task.deleted = true
      task.version += 1
      Store.record(actor.id, "delete", "task", task.id)
    end
    project
  end

  def restore_project(actor, project)
    raise Domain.conflict unless project.deleted

    project.deleted = false
    project.version += 1
    Store.record(actor.id, "restore", "project", project.id)
    project
  end

  # ------------------------------------------------------------------------ tasks

  def read_note(actor, body, errors, current)
    return current unless body.key?("internalNote")
    raise Domain.forbidden unless actor.role == "admin"

    note = body["internalNote"]
    raise Domain.bad_request unless note.is_a?(String)

    if note.length > Domain::MAX_TITLE_LENGTH
      errors << Domain.fail_with("internalNote", "internalNote is too long")
    end
    note
  end

  def create_task(actor, project, title, priority, assignee_id, note, errors)
    Domain.check_string(title, "title", Domain::MAX_TITLE_LENGTH, errors)
    Domain.check_priority(priority, errors)
    if !assignee_id.nil? && Store.find_user(assignee_id).nil?
      errors << Domain.fail_with("assigneeId", "assigneeId is not a known user")
    end
    raise Domain.invalid(errors) unless errors.empty?

    task = Store.insert_task(project.id, title, priority, assignee_id, note)
    Store.record(actor.id, "create", "task", task.id)
    task
  end

  def replace_task(actor, task, title, priority, assignee_id, note, errors)
    Domain.check_string(title, "title", Domain::MAX_TITLE_LENGTH, errors)
    Domain.check_priority(priority, errors)
    if !assignee_id.nil? && Store.find_user(assignee_id).nil?
      errors << Domain.fail_with("assigneeId", "assigneeId is not a known user")
    end
    raise Domain.invalid(errors) unless errors.empty?

    task.title = title
    task.priority = priority
    task.assignee_id = assignee_id
    task.internal_note = note
    task.version += 1
    Store.record(actor.id, "update", "task", task.id)
    task
  end

  def move_status(actor, task, status)
    errors = []
    Domain.check_status(status, errors)
    raise Domain.invalid(errors) unless errors.empty?
    raise Domain.invalid_transition unless Domain::TRANSITIONS.include?([task.status, status])

    task.status = status
    task.version += 1
    Store.record(actor.id, "update", "task", task.id)
    task
  end

  def delete_task(actor, task)
    task.deleted = true
    task.version += 1
    Store.record(actor.id, "delete", "task", task.id)
    task
  end

  def restore_task(actor, task)
    raise Domain.conflict unless task.deleted

    task.deleted = false
    task.version += 1
    Store.record(actor.id, "restore", "task", task.id)
    task
  end

  # --------------------------------------------------------------------- comments

  def create_comment(actor, task, body)
    errors = []
    Domain.check_string(body, "body", Domain::MAX_COMMENT_LENGTH, errors)
    raise Domain.invalid(errors) unless errors.empty?

    comment = Store.insert_comment(task.id, actor.id, body)
    Store.record(actor.id, "create", "comment", comment.id)
    comment
  end

  def remove_comment(actor, comment)
    raise Domain.forbidden if actor.role != "admin" && comment.author_id != actor.id

    Store.comments.delete(comment.id)
    Store.record(actor.id, "delete", "comment", comment.id)
  end

  # ------------------------------------------------------------------------ users

  def create_user(actor, username, password, role, quota)
    errors = []
    Domain.check_string(username, "username", Domain::MAX_NAME_LENGTH, errors)
    Domain.check_string(password, "password", Domain::MAX_NAME_LENGTH, errors)
    Domain.check_role(role, errors)
    Domain.check_quota(quota, errors)
    raise Domain.invalid(errors) unless errors.empty?
    raise Domain.conflict unless Store.find_by_username(username).nil?

    user = Store.insert_user(username, password, role, quota)
    Store.record(actor.id, "create", "user", user.id)
    user
  end

  def update_user(actor, user, body)
    errors = []
    Domain.check_role(body["role"], errors) if body.key?("role")
    Domain.check_quota(body["quota"], errors) if body.key?("quota")
    raise Domain.invalid(errors) unless errors.empty?

    user.role = body["role"] if body.key?("role")
    user.quota = body["quota"] if body.key?("quota")
    user.version += 1
    Store.record(actor.id, "update", "user", user.id)
    user
  end

  def delete_user(actor, user)
    raise Domain.conflict if user.id == actor.id

    user.deleted = true
    user.version += 1
    Store.record(actor.id, "delete", "user", user.id)
    user
  end

  # --------------------------------------------------------- queries and reports

  def visible_projects(user, include_deleted)
    Store.projects.values.select do |project|
      (include_deleted || !project.deleted) &&
        (user.role == "admin" || project.owner_id == user.id)
    end
  end

  def visible_tasks(user, include_deleted)
    allowed = visible_projects(user, true).map(&:id)
    Store.tasks.values.select do |task|
      allowed.include?(task.project_id) && (include_deleted || !task.deleted)
    end
  end

  def search(user, query)
    needle = query.downcase
    results = visible_projects(user, false)
              .select { |project| project.name.downcase.include?(needle) }
              .map { |project| { type: "project", id: project.id, label: project.name } }
    results += visible_tasks(user, false)
               .select { |task| task.title.downcase.include?(needle) }
               .map { |task| { type: "task", id: task.id, label: task.title } }
    { results: results, total: results.length }
  end

  def workload(user, group_by)
    rows = visible_tasks(user, false)
    groups = []
    case group_by
    when "status"
      Domain::STATUSES.each do |status|
        picked = rows.select { |task| task.status == status }
        groups << { key: status, tasks: picked.length,
                    totalScore: picked.sum { |task| Domain.compute_score(task.priority, task.status) } }
      end
    when "assignee"
      named = rows.filter_map(&:assignee_id).uniq.sort
      named.each do |assignee|
        picked = rows.select { |task| task.assignee_id == assignee }
        groups << { key: assignee.to_s, tasks: picked.length,
                    totalScore: picked.sum { |task| Domain.compute_score(task.priority, task.status) } }
      end
      loose = rows.select { |task| task.assignee_id.nil? }
      unless loose.empty?
        groups << { key: "unassigned", tasks: loose.length,
                    totalScore: loose.sum { |task| Domain.compute_score(task.priority, task.status) } }
      end
    else
      visible_projects(user, false).sort_by(&:id).each do |project|
        picked = rows.select { |task| task.project_id == project.id }
        groups << { key: project.name, tasks: picked.length,
                    totalScore: picked.sum { |task| Domain.compute_score(task.priority, task.status) } }
      end
    end
    { groupBy: group_by, groups: groups }
  end

  def flush_outbox
    pending = Store.outbox.reject(&:delivered)
    pending.each { |event| event.delivered = true }
    pending.length
  end

  def metrics
    { requests: Store.requests,
      byStatus: Store.by_status.keys.sort.to_h { |code| [code.to_s, Store.by_status[code]] },
      byRoute: Store.by_route.keys.sort.map { |route| { route: route, count: Store.by_route[route] } },
      auditEntries: Store.audit.length,
      outboxPending: Store.outbox_pending }
  end

  def stats
    live = Store.tasks.values.reject(&:deleted)
    counts = Domain::STATUSES.to_h { |status| [status, 0] }
    live.each { |task| counts[task.status] += 1 }
    total = live.length
    scores = live.map { |task| Domain.compute_score(task.priority, task.status) }
    best = nil
    Store.projects.each_value do |project|
      next if project.deleted

      best = project if best.nil? || Store.task_count(project.id) > Store.task_count(best.id)
    end
    { projects: Store.projects.values.count { |project| !project.deleted },
      tasks: total,
      users: Store.users.values.count { |user| !user.deleted },
      sessions: Store.sessions.length,
      comments: Store.comments.length,
      byStatus: counts,
      avgScore: total.zero? ? 0.0 : (scores.sum.to_f / total).round(2),
      topProjectName: best&.name,
      auditEntries: Store.audit.length,
      outboxPending: Store.outbox_pending }
  end

  def check_bulk_size(operations)
    return if operations.is_a?(Array) && operations.length.between?(1, Domain::MAX_BULK_ITEMS)

    raise Domain.invalid([Domain.fail_with("operations", "operations is out of range")])
  end
end

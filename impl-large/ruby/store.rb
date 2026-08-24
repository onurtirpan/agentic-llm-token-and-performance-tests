# frozen_string_literal: true

# Task Service, large tier — the in-memory state and its repositories.

require_relative "domain"

# `extend self` turns every method below into a module method, so the state can
# sit in the module's own instance variables and still be reached as Store.users
# from the layers above.
module Store
  extend self

  attr_reader :users, :sessions, :projects, :tasks, :comments, :audit, :outbox,
              :idempotency, :by_status, :by_route, :requests

  @users = {}
  @sessions = {}
  @projects = {}
  @tasks = {}
  @comments = {}
  @audit = []
  @outbox = []
  @idempotency = {}
  @by_status = {}
  @by_route = {}

  @requests = 0
  @next_project_id = 1
  @next_task_id = 1
  @next_comment_id = 1
  @next_user_id = 5
  @next_seq = 1

  def seed
    @users.update(
      1 => Domain::User.new(1, "admin", "admin-secret", "admin", Domain::DEFAULT_QUOTA, 1, false),
      2 => Domain::User.new(2, "alice", "alice-secret", "user", Domain::DEFAULT_QUOTA, 1, false),
      3 => Domain::User.new(3, "bob", "bob-secret", "user", Domain::DEFAULT_QUOTA, 1, false),
      4 => Domain::User.new(4, "probe", "probe-secret", "user", Domain::PROBE_QUOTA, 1, false)
    )
  end

  def take_seq
    value = @next_seq
    @next_seq += 1
    value
  end

  # Append one audit entry and one outbox event for a successful write.
  def record(actor_id, action, resource, resource_id)
    @audit << Domain::AuditEntry.new(take_seq, actor_id, action, resource, resource_id)
    @outbox << Domain::OutboxEvent.new(take_seq, "#{resource}.#{action}", resource_id, false)
  end

  def count_request(route, status)
    @requests += 1
    @by_route[route] = @by_route.fetch(route, 0) + 1
    @by_status[status] = @by_status.fetch(status, 0) + 1
  end

  def find_user(user_id, include_deleted = false)
    user = @users[user_id]
    user if user && (include_deleted || !user.deleted)
  end

  def find_by_username(username)
    @users.values.find { |user| user.username == username && !user.deleted }
  end

  def insert_user(username, password, role, quota)
    user = Domain::User.new(@next_user_id, username, password, role, quota, 1, false)
    @users[user.id] = user
    @next_user_id += 1
    user
  end

  def find_project(project_id, include_deleted = false)
    project = @projects[project_id]
    project if project && (include_deleted || !project.deleted)
  end

  def insert_project(name, owner_id)
    project = Domain::Project.new(@next_project_id, name, owner_id, 1, false)
    @projects[project.id] = project
    @next_project_id += 1
    project
  end

  def find_task(task_id, include_deleted = false)
    task = @tasks[task_id]
    task if task && (include_deleted || !task.deleted)
  end

  def insert_task(project_id, title, priority, assignee_id, internal_note)
    task = Domain::Task.new(@next_task_id, project_id, title, priority, "todo",
                            assignee_id, internal_note, 1, false)
    @tasks[task.id] = task
    @next_task_id += 1
    task
  end

  def find_comment(comment_id)
    @comments[comment_id]
  end

  def insert_comment(task_id, author_id, body)
    comment = Domain::Comment.new(@next_comment_id, task_id, author_id, body)
    @comments[comment.id] = comment
    @next_comment_id += 1
    comment
  end

  def live_tasks_of(project_id)
    @tasks.values.select { |task| task.project_id == project_id && !task.deleted }
  end

  def task_count(project_id)
    live_tasks_of(project_id).length
  end

  def outbox_pending
    @outbox.count { |event| !event.delivered }
  end
end

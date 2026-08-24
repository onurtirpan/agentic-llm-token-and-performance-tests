# frozen_string_literal: true

# Task Service, large tier — HTTP routing, middleware and the entry point.

require "json"
require "securerandom"
require "sinatra"

require_relative "domain"
require_relative "service"
require_relative "store"

set :bind, "127.0.0.1"
set :port, Domain::PORT
set :logging, false
set :show_exceptions, false
set :raise_errors, false

# The structured log is the only thing allowed on stdout, so the server starts
# quietly, never dumps a backtrace, and flushes every line as it is written.
set :dump_errors, false
set :quiet, true
set :server_settings, { Silent: true }
$stdout.sync = true

Store.seed

# ------------------------------------------------------------------- middleware

before do
  content_type :json
  given_id = request.env["HTTP_X_REQUEST_ID"].to_s
  @request_id = given_id.empty? ? SecureRandom.hex(6) : given_id
  @user_id = nil
  @quota_remaining = nil
  @audit_before = Store.audit.length
  @started = Process.clock_gettime(Process::CLOCK_MONOTONIC)
  headers "X-Request-Id" => @request_id
end

after do
  status_code = response.status
  level = if status_code >= 500
            "error"
          elsif status_code >= 400
            "warn"
          else
            "info"
          end
  duration_ms = (Process.clock_gettime(Process::CLOCK_MONOTONIC) - @started) * 1000
  Store.count_request(route_label, status_code)
  $stdout.puts({ level: level, requestId: @request_id, method: request.request_method,
                 path: request.path, status: status_code, durationMs: duration_ms.to_i,
                 userId: @user_id, quotaRemaining: @quota_remaining,
                 auditSeq: Store.audit.length - @audit_before }.to_json)
end

error Domain::AppError do
  error = env["sinatra.error"]
  status error.status
  envelope(error).to_json
end

# ---------------------------------------------------------------------- helpers

helpers do
  def envelope(error)
    { error: { code: error.code, message: error.message,
               requestId: @request_id, details: error.details } }
  end

  # Sinatra reports a match as "GET /projects/:id", while the specification
  # spells the same pattern "GET /projects/{id}". The catch-all is not a real
  # pattern, so an unmatched path carries the literal label "unmatched".
  def route_label
    matched = request.env["sinatra.route"]
    return "unmatched" if matched.nil? || matched.end_with?("/*")

    matched.gsub(/:(\w+)/, '{\1}')
  end

  # Authenticate, charge the quota, then check the role. This order is fixed.
  # Named begin_with, not begin, because begin is a Ruby keyword.
  def begin_with(admin: false)
    user, session = Service.authenticate(request.env["HTTP_AUTHORIZATION"].to_s)
    @user_id = user.id
    @quota_remaining = Service.charge_quota(user, session)
    headers "X-Quota-Remaining" => @quota_remaining.to_s
    Service.require_admin(user) if admin
    [user, session]
  end

  def body_of
    raw = request.body.read
    return {} if raw.strip.empty?

    parsed = JSON.parse(raw)
    raise Domain.bad_request unless parsed.is_a?(Hash)

    parsed
  rescue JSON::ParserError
    raise Domain.bad_request
  end

  def whole(body, field, default)
    value = body.fetch(field, default)
    raise Domain.bad_request unless value.nil? || value.is_a?(Integer)

    value
  end

  def text(body, field, default = "")
    value = body.fetch(field, default)
    raise Domain.bad_request unless value.is_a?(String)

    value
  end

  def parse_id(raw)
    raise Domain.bad_request unless raw.match?(/\A-?\d+\z/)

    raw.to_i
  end

  def read_page(allowed)
    errors = []
    limit = Domain::DEFAULT_LIMIT
    offset = 0
    sort = params.fetch("sort", allowed.first)
    order = params.fetch("order", "asc")
    if params.key?("limit")
      limit = params["limit"].match?(/\A-?\d+\z/) ? params["limit"].to_i : -1
      if limit < 1 || limit > Domain::MAX_LIMIT
        errors << Domain.fail_with("limit", "limit is out of range")
      end
    end
    if params.key?("offset")
      offset = params["offset"].match?(/\A-?\d+\z/) ? params["offset"].to_i : -1
      errors << Domain.fail_with("offset", "offset is out of range") if offset.negative?
    end
    errors << Domain.fail_with("sort", "sort is not a valid field") unless allowed.include?(sort)
    unless %w[asc desc].include?(order)
      errors << Domain.fail_with("order", "order must be asc or desc")
    end
    raise Domain.invalid(errors) unless errors.empty?

    [limit, offset, sort, order]
  end

  # Sinatra's own etag helper quotes the value and answers If-Match by itself,
  # so the header is written directly instead.
  def tagged(body, version)
    headers "ETag" => version.to_s
    body.to_json
  end

  def if_match(version)
    Service.check_if_match(request.env["HTTP_IF_MATCH"], version)
  end

  # A single-resource body carries its version, so the ETag comes for free.
  def responded(status_code, body)
    status status_code
    headers "ETag" => body[:version].to_s if body.key?(:version)
    body.to_json
  end

  # Run the block once per Idempotency-Key, then replay the recorded outcome.
  def idempotent(session)
    key = request.env["HTTP_IDEMPOTENCY_KEY"]
    return responded(*yield) if key.nil?

    slot = [session.token, key]
    recorded = Store.idempotency[slot]
    if recorded
      headers "Idempotency-Replayed" => "true"
      return responded(*recorded)
    end
    begin
      status_code, body = yield
    rescue Domain::AppError => error
      Store.idempotency[slot] = [error.status, envelope(error)]
      raise
    end
    Store.idempotency[slot] = [status_code, body]
    responded(status_code, body)
  end

  # The status filter is read straight from params, because a local named status
  # would shadow the Sinatra status helper for the rest of this method.
  def task_filters(rows)
    errors = []
    assignee = params["assigneeId"]
    if params.key?("status") && !Domain::STATUSES.include?(params["status"])
      errors << Domain.fail_with("status", "status is not valid")
    end
    if assignee && !assignee.match?(/\A-?\d+\z/)
      errors << Domain.fail_with("assigneeId", "assigneeId is not a known user")
    end
    raise Domain.invalid(errors) unless errors.empty?

    rows = rows.select { |task| task.status == params["status"] } if params.key?("status")
    rows = rows.select { |task| task.assignee_id == assignee.to_i } if assignee
    rows
  end

  def apply_bulk(actor, item)
    case item["op"]
    when "create"
      project = Service.reachable_project(whole(item, "projectId", 0), actor)
      task = Service.create_task(actor, project, text(item, "title"),
                                 whole(item, "priority", 0), nil, "", [])
      { status: 201, id: task.id, error: nil }
    when "status"
      task = Service.reachable_task(whole(item, "id", 0), actor)
      Service.check_if_match(item["version"].to_s, task.version)
      Service.move_status(actor, task, item["status"])
      { status: 200, id: task.id, error: nil }
    when "delete"
      task = Service.reachable_task(whole(item, "id", 0), actor)
      Service.check_if_match(item["version"].to_s, task.version)
      Service.delete_task(actor, task)
      { status: 200, id: task.id, error: nil }
    else
      raise Domain.invalid([Domain.fail_with("op", "op is not valid")])
    end
  end
end

# ------------------------------------------------------------------ health, auth

get "/health" do
  { status: "ok",
    projects: Store.projects.values.count { |project| !project.deleted },
    tasks: Store.tasks.values.count { |task| !task.deleted },
    comments: Store.comments.length }.to_json
end

post "/auth/login" do
  body = body_of
  errors = []
  username = text(body, "username")
  password = text(body, "password")
  errors << Domain.fail_with("username", "username is required") if username.empty?
  errors << Domain.fail_with("password", "password is required") if password.empty?
  raise Domain.invalid(errors) unless errors.empty?

  token = SecureRandom.hex(16)
  user = Service.login(username, password, token)
  { token: token, userId: user.id, role: user.role }.to_json
end

post "/auth/logout" do
  _user, session = begin_with
  Store.sessions.delete(session.token)
  status 204
  ""
end

get "/me" do
  user, _session = begin_with
  { userId: user.id, username: user.username, role: user.role }.to_json
end

# ------------------------------------------------------------------------ users

get "/users" do
  begin_with(admin: true)
  limit, offset, sort, order = read_page(Domain::USER_SORTS)
  rows = Store.users.values.reject(&:deleted).map { |user| Service.serialize_user(user) }
  Service.paginate(rows, limit, offset, sort, order).to_json
end

post "/users" do
  actor, session = begin_with(admin: true)
  body = body_of
  idempotent(session) do
    user = Service.create_user(actor, text(body, "username"), text(body, "password"),
                               body.fetch("role", "user"),
                               body.fetch("quota", Domain::DEFAULT_QUOTA))
    [201, Service.serialize_user(user)]
  end
end

get "/users/:id" do
  begin_with(admin: true)
  user = Store.find_user(parse_id(params["id"]))
  raise Domain.not_found if user.nil?

  tagged(Service.serialize_user(user), user.version)
end

patch "/users/:id" do
  actor, _session = begin_with(admin: true)
  user = Store.find_user(parse_id(params["id"]))
  raise Domain.not_found if user.nil?

  if_match(user.version)
  Service.update_user(actor, user, body_of)
  tagged(Service.serialize_user(user), user.version)
end

delete "/users/:id" do
  actor, _session = begin_with(admin: true)
  user = Store.find_user(parse_id(params["id"]))
  raise Domain.not_found if user.nil?

  if_match(user.version)
  Service.delete_user(actor, user)
  tagged(Service.serialize_user(user), user.version)
end

# --------------------------------------------------------------------- projects

get "/projects" do
  user, _session = begin_with
  include_deleted = Service.check_include_deleted(params["includeDeleted"], user)
  limit, offset, sort, order = read_page(Domain::PROJECT_SORTS)
  rows = Service.visible_projects(user, include_deleted)
                .map { |project| Service.serialize_project(project) }
  Service.paginate(rows, limit, offset, sort, order).to_json
end

post "/projects" do
  actor, session = begin_with(admin: true)
  body = body_of
  idempotent(session) do
    project = Service.create_project(actor, text(body, "name"),
                                     whole(body, "ownerId", actor.id))
    [201, Service.serialize_project(project)]
  end
end

get "/projects/:id" do
  user, _session = begin_with
  project = Service.reachable_project(parse_id(params["id"]), user)
  tagged(Service.serialize_project(project), project.version)
end

patch "/projects/:id" do
  actor, _session = begin_with(admin: true)
  project = Service.reachable_project(parse_id(params["id"]), actor)
  if_match(project.version)
  body = body_of
  Service.rename_project(actor, project, text(body, "name")) if body.key?("name")
  tagged(Service.serialize_project(project), project.version)
end

delete "/projects/:id" do
  actor, _session = begin_with(admin: true)
  project = Service.reachable_project(parse_id(params["id"]), actor)
  if_match(project.version)
  Service.delete_project(actor, project)
  tagged(Service.serialize_project(project), project.version)
end

post "/projects/:id/restore" do
  actor, _session = begin_with(admin: true)
  project = Service.reachable_project(parse_id(params["id"]), actor, true)
  if_match(project.version)
  Service.restore_project(actor, project)
  tagged(Service.serialize_project(project), project.version)
end

# ------------------------------------------------------------------------ tasks

get "/tasks" do
  user, _session = begin_with
  include_deleted = Service.check_include_deleted(params["includeDeleted"], user)
  limit, offset, sort, order = read_page(Domain::TASK_SORTS)
  rows = task_filters(Service.visible_tasks(user, include_deleted))
  Service.paginate(rows.map { |task| Service.serialize_task(task, user.role) },
                   limit, offset, sort, order).to_json
end

get "/projects/:id/tasks" do
  user, _session = begin_with
  project = Service.reachable_project(parse_id(params["id"]), user)
  limit, offset, sort, order = read_page(Domain::TASK_SORTS)
  rows = Store.tasks.values.select { |task| task.project_id == project.id && !task.deleted }
  Service.paginate(rows.map { |task| Service.serialize_task(task, user.role) },
                   limit, offset, sort, order).to_json
end

post "/projects/:id/tasks" do
  actor, session = begin_with
  project = Service.reachable_project(parse_id(params["id"]), actor)
  body = body_of
  idempotent(session) do
    errors = []
    note = Service.read_note(actor, body, errors, "")
    task = Service.create_task(actor, project, text(body, "title"),
                               whole(body, "priority", 0),
                               whole(body, "assigneeId", nil), note, errors)
    [201, Service.serialize_task(task, actor.role)]
  end
end

get "/tasks/:id" do
  user, _session = begin_with
  task = Service.reachable_task(parse_id(params["id"]), user)
  tagged(Service.serialize_task(task, user.role), task.version)
end

put "/tasks/:id" do
  actor, _session = begin_with
  task = Service.reachable_task(parse_id(params["id"]), actor)
  if_match(task.version)
  body = body_of
  errors = []
  note = Service.read_note(actor, body, errors, task.internal_note)
  Service.replace_task(actor, task, text(body, "title"), whole(body, "priority", 0),
                       whole(body, "assigneeId", nil), note, errors)
  tagged(Service.serialize_task(task, actor.role), task.version)
end

patch "/tasks/:id/status" do
  actor, _session = begin_with
  task = Service.reachable_task(parse_id(params["id"]), actor)
  if_match(task.version)
  Service.move_status(actor, task, body_of["status"])
  tagged(Service.serialize_task(task, actor.role), task.version)
end

delete "/tasks/:id" do
  actor, _session = begin_with
  task = Service.reachable_task(parse_id(params["id"]), actor)
  if_match(task.version)
  Service.delete_task(actor, task)
  tagged(Service.serialize_task(task, actor.role), task.version)
end

post "/tasks/:id/restore" do
  actor, _session = begin_with
  task = Service.reachable_task(parse_id(params["id"]), actor, true)
  if_match(task.version)
  Service.restore_task(actor, task)
  tagged(Service.serialize_task(task, actor.role), task.version)
end

post "/tasks/bulk" do
  actor, _session = begin_with
  operations = body_of["operations"]
  Service.check_bulk_size(operations)
  results = operations.each_with_index.map do |item, index|
    raise Domain.bad_request unless item.is_a?(Hash)

    { index: index }.merge(apply_bulk(actor, item))
  rescue Domain::AppError => error
    { index: index, status: error.status, id: nil, error: error.code }
  end
  { results: results }.to_json
end

# --------------------------------------------------------------------- comments

get "/tasks/:id/comments" do
  user, _session = begin_with
  task = Service.reachable_task(parse_id(params["id"]), user)
  limit, offset, sort, order = read_page(Domain::COMMENT_SORTS)
  rows = Store.comments.values.select { |comment| comment.task_id == task.id }
              .map { |comment| Service.serialize_comment(comment) }
  Service.paginate(rows, limit, offset, sort, order).to_json
end

post "/tasks/:id/comments" do
  actor, session = begin_with
  task = Service.reachable_task(parse_id(params["id"]), actor)
  body = body_of
  idempotent(session) do
    comment = Service.create_comment(actor, task, text(body, "body"))
    [201, Service.serialize_comment(comment)]
  end
end

delete "/comments/:id" do
  actor, _session = begin_with
  comment = Store.find_comment(parse_id(params["id"]))
  raise Domain.not_found if comment.nil?

  Service.reachable_task(comment.task_id, actor, true)
  Service.remove_comment(actor, comment)
  status 204
  ""
end

# --------------------------------------------------- search, reports, telemetry

get "/search" do
  user, _session = begin_with
  query = params.fetch("q", "")
  raise Domain.invalid([Domain.fail_with("q", "q is required")]) if query.empty?

  Service.search(user, query).to_json
end

get "/reports/workload" do
  user, _session = begin_with
  group_by = params.fetch("groupBy", "status")
  unless Domain::GROUP_BYS.include?(group_by)
    raise Domain.invalid([Domain.fail_with("groupBy", "groupBy is not valid")])
  end

  Service.workload(user, group_by).to_json
end

get "/audit" do
  begin_with(admin: true)
  limit, offset, sort, order = read_page(Domain::SEQ_SORTS)
  rows = Store.audit.select do |entry|
    (params["actorId"].nil? || entry.actor_id.to_s == params["actorId"]) &&
      (params["resource"].nil? || entry.resource == params["resource"]) &&
      (params["action"].nil? || entry.action == params["action"])
  end
  Service.paginate(rows.map { |entry| Service.serialize_audit(entry) },
                   limit, offset, sort, order).to_json
end

get "/outbox" do
  begin_with(admin: true)
  limit, offset, sort, order = read_page(Domain::SEQ_SORTS)
  wanted = params["delivered"]
  rows = Store.outbox.select { |event| wanted.nil? || event.delivered == (wanted == "true") }
  Service.paginate(rows.map { |event| Service.serialize_outbox(event) },
                   limit, offset, sort, order).to_json
end

post "/outbox/flush" do
  begin_with(admin: true)
  { flushed: Service.flush_outbox }.to_json
end

get "/metrics" do
  begin_with(admin: true)
  Service.metrics.to_json
end

get "/stats" do
  begin_with(admin: true)
  Service.stats.to_json
end

# A catch-all route, not a `not_found` block: Sinatra runs an error block for
# every 404 response, which would overwrite the envelope of a matched route.
%w[get post put patch delete].each do |verb|
  send(verb, "/*") { raise Domain.not_found }
end

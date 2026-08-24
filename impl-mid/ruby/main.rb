# Task Service, mid tier — Sinatra implementation.

require "json"
require "securerandom"
require "sinatra"

MAX_TITLE_LENGTH = 80
MAX_NAME_LENGTH = 60
MIN_PRIORITY = 1
MAX_PRIORITY = 5
DEFAULT_LIMIT = 20
MAX_LIMIT = 100
PORT = 8080

STATUS_BONUS = { "todo" => 0, "in_progress" => 3, "done" => 5, "archived" => 0 }.freeze
TRANSITIONS = [%w[todo in_progress], %w[todo archived], %w[in_progress todo],
               %w[in_progress done], %w[done archived]].freeze
PROJECT_SORTS = %w[id name taskCount].freeze
TASK_SORTS = %w[id title priority score status].freeze

set :bind, "127.0.0.1"
set :port, PORT
set :logging, false
set :show_exceptions, false
set :raise_errors, false

# The structured log is the only thing allowed on stdout, and it must survive an
# abrupt stop, so the server stays quiet and every line leaves the buffer at once.
set :quiet, true
set :server_settings, { Silent: true }
$stdout.sync = true

User = Struct.new(:id, :username, :password, :role)
Project = Struct.new(:id, :name, :owner_id)
Task = Struct.new(:id, :project_id, :title, :priority, :status, :assignee_id, :score)

# A helper is a method, so it cannot close over the local variables of this file
# the way a route block does. The whole store therefore lives in one object.
class Store
  attr_reader :users, :sessions, :projects, :tasks
  attr_accessor :next_project_id, :next_task_id

  def initialize
    @users = { 1 => User.new(1, "admin", "admin-secret", "admin"),
               2 => User.new(2, "alice", "alice-secret", "user"),
               3 => User.new(3, "bob", "bob-secret", "user") }
    @sessions = {}
    @projects = {}
    @tasks = {}
    @next_project_id = 1
    @next_task_id = 1
  end
end

STORE = Store.new

def compute_score(priority, status)
  base_score = priority * 10
  base_score + STATUS_BONUS[status]
end

def task_count(project_id)
  STORE.tasks.values.count { |task| task.project_id == project_id }
end

def serialize_project(project)
  { id: project.id, name: project.name, ownerId: project.owner_id,
    taskCount: task_count(project.id) }
end

def serialize_task(task)
  { id: task.id, projectId: task.project_id, title: task.title, priority: task.priority,
    status: task.status, assigneeId: task.assignee_id, score: task.score }
end

# Named fail_with, not fail, because Kernel#fail is a Ruby built-in.
def fail_with(field, message)
  { field: field, message: message }
end

def paginate(rows, limit, offset, sort, order)
  key = sort.to_sym
  # Ruby does not sort stably, so the id tie-break is spelled out here.
  sorted = rows.sort do |left, right|
    ranking = left[key] <=> right[key]
    ranking = -ranking if order == "desc"
    ranking.zero? ? left[:id] <=> right[:id] : ranking
  end
  { items: sorted[offset, limit] || [], total: sorted.length,
    limit: limit, offset: offset }
end

helpers do
  def app_error(status_code, code, message, details = [])
    halt status_code, { error: { code: code, message: message,
                                 requestId: @request_id, details: details } }.to_json
  end

  def bad_request
    app_error(400, "bad_request", "the request is malformed")
  end

  def not_found
    app_error(404, "not_found", "the resource does not exist")
  end

  def forbidden
    app_error(403, "forbidden", "you may not access this resource")
  end

  def conflict
    app_error(409, "conflict", "the resource already exists")
  end

  def invalid(details)
    details.sort_by! { |entry| [entry[:field], entry[:message]] }
    app_error(422, "validation_failed", "the request body is not valid", details)
  end

  def read_body
    raw = request.body.read
    return {} if raw.strip.empty?

    parsed = JSON.parse(raw)
    bad_request unless parsed.is_a?(Hash)
    parsed
  rescue JSON::ParserError
    bad_request
  end

  def read_int(body, field, default)
    value = body.fetch(field, default)
    bad_request unless value.nil? || value.is_a?(Integer)
    value
  end

  def read_string(body, field, errors, max_length, required)
    value = body.fetch(field, "")
    bad_request unless value.is_a?(String)
    if value.empty?
      errors << fail_with(field, "#{field} is required") if required
    elsif value.length > max_length
      errors << fail_with(field, "#{field} is too long")
    end
    value
  end

  def read_priority(body, errors)
    value = read_int(body, "priority", 0)
    if value.nil? || value < MIN_PRIORITY || value > MAX_PRIORITY
      errors << fail_with("priority", "priority is out of range")
    end
    value || 0
  end

  def read_user_ref(body, field, errors, default, nullable)
    value = read_int(body, field, default)
    return nil if value.nil? && nullable

    errors << fail_with(field, "#{field} is not a known user") unless STORE.users.key?(value)
    value
  end

  def parse_id(raw)
    bad_request unless raw.match?(/\A-?\d+\z/)
    raw.to_i
  end

  def read_page(allowed)
    errors = []
    limit = DEFAULT_LIMIT
    offset = 0
    sort = params.fetch("sort", "id")
    order = params.fetch("order", "asc")
    if params.key?("limit")
      raw = params["limit"]
      limit = raw.match?(/\A-?\d+\z/) ? raw.to_i : -1
      errors << fail_with("limit", "limit is out of range") if limit < 1 || limit > MAX_LIMIT
    end
    if params.key?("offset")
      raw = params["offset"]
      offset = raw.match?(/\A-?\d+\z/) ? raw.to_i : -1
      errors << fail_with("offset", "offset is out of range") if offset.negative?
    end
    errors << fail_with("sort", "sort is not a valid field") unless allowed.include?(sort)
    errors << fail_with("order", "order must be asc or desc") unless %w[asc desc].include?(order)
    invalid(errors) unless errors.empty?

    [limit, offset, sort, order]
  end

  def authenticate
    header = request.env["HTTP_AUTHORIZATION"].to_s
    @user_id = STORE.sessions[header.delete_prefix("Bearer ")] if header.start_with?("Bearer ")
    app_error(401, "unauthorized", "authentication is required") if @user_id.nil?
    STORE.users[@user_id]
  end

  def require_admin(user)
    forbidden unless user.role == "admin"
  end

  def reachable_project(project_id, user)
    project = STORE.projects[project_id]
    not_found if project.nil?
    forbidden if user.role != "admin" && project.owner_id != user.id
    project
  end

  def reachable_task(task_id, user)
    task = STORE.tasks[task_id]
    not_found if task.nil?
    reachable_project(task.project_id, user)
    task
  end
end

before do
  content_type :json
  given_id = request.env["HTTP_X_REQUEST_ID"].to_s
  @request_id = given_id.empty? ? SecureRandom.hex(6) : given_id
  @user_id = nil
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
  $stdout.puts({ level: level, requestId: @request_id, method: request.request_method,
                 path: request.path, status: status_code, durationMs: duration_ms.to_i,
                 userId: @user_id }.to_json)
end

get "/health" do
  { status: "ok", projects: STORE.projects.size, tasks: STORE.tasks.size }.to_json
end

post "/auth/login" do
  body = read_body
  errors = []
  username = read_string(body, "username", errors, MAX_NAME_LENGTH, true)
  password = read_string(body, "password", errors, MAX_NAME_LENGTH, true)
  invalid(errors) unless errors.empty?

  user = STORE.users.values.find do |candidate|
    candidate.username == username && candidate.password == password
  end
  app_error(401, "invalid_credentials", "the username or password is wrong") if user.nil?
  token = SecureRandom.hex(16)
  STORE.sessions[token] = user.id
  { token: token, userId: user.id, role: user.role }.to_json
end

post "/auth/logout" do
  authenticate
  STORE.sessions.delete(request.env["HTTP_AUTHORIZATION"].delete_prefix("Bearer "))
  status 204
  ""
end

get "/me" do
  user = authenticate
  { userId: user.id, username: user.username, role: user.role }.to_json
end

get "/projects" do
  user = authenticate
  limit, offset, sort, order = read_page(PROJECT_SORTS)
  rows = STORE.projects.values
              .select { |project| user.role == "admin" || project.owner_id == user.id }
              .map { |project| serialize_project(project) }
  paginate(rows, limit, offset, sort, order).to_json
end

post "/projects" do
  user = authenticate
  require_admin(user)
  body = read_body
  errors = []
  name = read_string(body, "name", errors, MAX_NAME_LENGTH, true)
  owner_id = read_user_ref(body, "ownerId", errors, user.id, false)
  invalid(errors) unless errors.empty?
  duplicate = STORE.projects.values.any? do |other|
    other.owner_id == owner_id && other.name == name
  end
  conflict if duplicate

  project = Project.new(STORE.next_project_id, name, owner_id)
  STORE.projects[project.id] = project
  STORE.next_project_id += 1
  status 201
  serialize_project(project).to_json
end

get "/projects/:id" do
  user = authenticate
  serialize_project(reachable_project(parse_id(params["id"]), user)).to_json
end

patch "/projects/:id" do
  user = authenticate
  require_admin(user)
  project = reachable_project(parse_id(params["id"]), user)
  body = read_body
  if body.key?("name")
    errors = []
    name = read_string(body, "name", errors, MAX_NAME_LENGTH, true)
    invalid(errors) unless errors.empty?
    duplicate = STORE.projects.values.any? do |other|
      other.owner_id == project.owner_id && other.name == name && other.id != project.id
    end
    conflict if duplicate
    project.name = name
  end
  serialize_project(project).to_json
end

delete "/projects/:id" do
  user = authenticate
  require_admin(user)
  project = reachable_project(parse_id(params["id"]), user)
  STORE.tasks.delete_if { |_id, task| task.project_id == project.id }
  STORE.projects.delete(project.id)
  status 204
  ""
end

get "/projects/:id/tasks" do
  user = authenticate
  project = reachable_project(parse_id(params["id"]), user)
  limit, offset, sort, order = read_page(TASK_SORTS)
  rows = STORE.tasks.values
              .select { |task| task.project_id == project.id }
              .map { |task| serialize_task(task) }
  paginate(rows, limit, offset, sort, order).to_json
end

post "/projects/:id/tasks" do
  user = authenticate
  project = reachable_project(parse_id(params["id"]), user)
  body = read_body
  errors = []
  title = read_string(body, "title", errors, MAX_TITLE_LENGTH, true)
  priority = read_priority(body, errors)
  assignee_id = read_user_ref(body, "assigneeId", errors, nil, true)
  invalid(errors) unless errors.empty?

  task = Task.new(STORE.next_task_id, project.id, title, priority, "todo", assignee_id,
                  compute_score(priority, "todo"))
  STORE.tasks[task.id] = task
  STORE.next_task_id += 1
  status 201
  serialize_task(task).to_json
end

get "/tasks/:id" do
  user = authenticate
  serialize_task(reachable_task(parse_id(params["id"]), user)).to_json
end

put "/tasks/:id" do
  user = authenticate
  task = reachable_task(parse_id(params["id"]), user)
  body = read_body
  errors = []
  title = read_string(body, "title", errors, MAX_TITLE_LENGTH, true)
  priority = read_priority(body, errors)
  assignee_id = read_user_ref(body, "assigneeId", errors, nil, true)
  invalid(errors) unless errors.empty?

  task.title = title
  task.priority = priority
  task.assignee_id = assignee_id
  task.score = compute_score(priority, task.status)
  serialize_task(task).to_json
end

patch "/tasks/:id/status" do
  user = authenticate
  task = reachable_task(parse_id(params["id"]), user)
  body = read_body
  # Named new_status, not status, because status is the Sinatra response helper.
  new_status = body["status"]
  invalid([fail_with("status", "status is not valid")]) unless STATUS_BONUS.key?(new_status)
  unless TRANSITIONS.include?([task.status, new_status])
    app_error(409, "invalid_transition", "the status change is not allowed")
  end

  task.status = new_status
  task.score = compute_score(task.priority, new_status)
  serialize_task(task).to_json
end

delete "/tasks/:id" do
  user = authenticate
  task = reachable_task(parse_id(params["id"]), user)
  STORE.tasks.delete(task.id)
  status 204
  ""
end

get "/stats" do
  user = authenticate
  require_admin(user)
  by_status = STATUS_BONUS.transform_values { 0 }
  STORE.tasks.each_value { |task| by_status[task.status] += 1 }
  total = STORE.tasks.size
  sum_score = STORE.tasks.values.sum(&:score)
  avg_score = total.zero? ? 0.0 : (sum_score.to_f / total).round(2)

  best = nil
  STORE.projects.each_value do |project|
    best = project if best.nil? || task_count(project.id) > task_count(best.id)
  end

  { projects: STORE.projects.size, tasks: total, users: STORE.users.size,
    sessions: STORE.sessions.size, byStatus: by_status, avgScore: avg_score,
    topProjectName: best&.name }.to_json
end

# A catch-all route, not a `not_found` block: Sinatra runs an error block for
# every 404 response, which would overwrite the envelope of a matched route.
%w[get post put patch delete].each do |verb|
  send(verb, "/*") { not_found }
end

# Task Service — Sinatra implementation.

require "sinatra"
require "json"

MAX_TITLE_LENGTH = 80
MIN_PRIORITY = 1
MAX_PRIORITY = 5
PORT = 8080

set :bind, "127.0.0.1"
set :port, PORT
set :logging, false
set :show_exceptions, false
set :raise_errors, false

tasks = {}
next_id = 1

def compute_score(priority, done)
  base_score = priority * 10
  done ? base_score : base_score + 5
end

def validate(title, priority)
  return "title is required" if title.empty?
  return "title is too long" if title.length > MAX_TITLE_LENGTH
  return "priority is out of range" if priority < MIN_PRIORITY || priority > MAX_PRIORITY

  nil
end

helpers do
  # Named fail_with, not fail, because Kernel#fail is a Ruby built-in.
  def fail_with(status_code, message)
    halt status_code, { error: message }.to_json
  end

  def parse_id(raw)
    raw.match?(/\A-?\d+\z/) ? raw.to_i : nil
  end

  def read_input(body)
    parsed = JSON.parse(body)
    return nil unless parsed.is_a?(Hash)

    title = parsed.fetch("title", "")
    priority = parsed.fetch("priority", 0)
    done = parsed.fetch("done", false)
    return nil unless title.is_a?(String) && priority.is_a?(Integer)
    return nil unless [true, false].include?(done)

    { title: title, priority: priority, done: done }
  rescue JSON::ParserError
    nil
  end
end

before { content_type :json }

get "/health" do
  { status: "ok", count: tasks.size }.to_json
end

get "/tasks" do
  done = params["done"]
  fail_with(400, "done must be true or false") if done && done != "true" && done != "false"

  selected = tasks.values.select { |task| done.nil? || task[:done] == (done == "true") }
  selected.sort_by! { |task| [-task[:score], task[:id]] }
  { tasks: selected, total: selected.length }.to_json
end

get "/tasks/:id" do
  id = parse_id(params["id"])
  fail_with(400, "invalid id") if id.nil?
  task = tasks[id]
  fail_with(404, "task not found") if task.nil?
  task.to_json
end

post "/tasks" do
  input = read_input(request.body.read)
  fail_with(400, "invalid json") if input.nil?
  error = validate(input[:title], input[:priority])
  fail_with(400, error) if error

  task = { id: next_id, title: input[:title], priority: input[:priority],
           done: false, score: compute_score(input[:priority], false) }
  tasks[next_id] = task
  next_id += 1
  status 201
  task.to_json
end

put "/tasks/:id" do
  id = parse_id(params["id"])
  fail_with(400, "invalid id") if id.nil?
  task = tasks[id]
  fail_with(404, "task not found") if task.nil?
  input = read_input(request.body.read)
  fail_with(400, "invalid json") if input.nil?
  error = validate(input[:title], input[:priority])
  fail_with(400, error) if error

  task[:title] = input[:title]
  task[:priority] = input[:priority]
  task[:done] = input[:done]
  task[:score] = compute_score(input[:priority], input[:done])
  task.to_json
end

delete "/tasks/:id" do
  id = parse_id(params["id"])
  fail_with(400, "invalid id") if id.nil?
  fail_with(404, "task not found") if tasks.delete(id).nil?
  status 204
  ""
end

get "/stats" do
  all = tasks.values
  total = all.length
  done_count = all.count { |task| task[:done] }
  sum_score = all.sum { |task| task[:score] }
  avg_score = total.zero? ? 0 : (sum_score.to_f / total).round(2)

  best = nil
  all.each do |task|
    next if task[:done]

    best = task if best.nil? || task[:priority] > best[:priority]
  end

  { total: total, doneCount: done_count, openCount: total - done_count,
    avgScore: avg_score, topOpenTitle: best&.fetch(:title) }.to_json
end

# A catch-all route, not a `not_found` block: Sinatra runs an error block for
# every 404 response, which would overwrite "task not found" with "not found".
%w[get post put delete].each do |verb|
  send(verb, "/*") { fail_with(404, "not found") }
end

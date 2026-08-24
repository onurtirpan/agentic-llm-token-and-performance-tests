# frozen_string_literal: true

# Task Service, large tier — domain types, constants and pure rules.

module Domain
  MAX_TITLE_LENGTH = 80
  MAX_NAME_LENGTH = 60
  MAX_COMMENT_LENGTH = 200
  MAX_BULK_ITEMS = 20
  MIN_PRIORITY = 1
  MAX_PRIORITY = 5
  DEFAULT_LIMIT = 20
  MAX_LIMIT = 100
  DEFAULT_QUOTA = 10_000
  PROBE_QUOTA = 5
  PORT = 8080

  ROLES = %w[admin user].freeze
  STATUSES = %w[todo in_progress done archived].freeze
  STATUS_BONUS = { "todo" => 0, "in_progress" => 3, "done" => 5, "archived" => 0 }.freeze
  TRANSITIONS = [%w[todo in_progress], %w[todo archived], %w[in_progress todo],
                 %w[in_progress done], %w[done archived]].freeze
  ACTIONS = %w[create update delete restore].freeze
  PROJECT_SORTS = %w[id name taskCount].freeze
  TASK_SORTS = %w[id title priority score status].freeze
  USER_SORTS = %w[id username role].freeze
  COMMENT_SORTS = %w[id authorId].freeze
  SEQ_SORTS = %w[seq].freeze
  GROUP_BYS = %w[assignee status project].freeze

  User = Struct.new(:id, :username, :password, :role, :quota, :version, :deleted)
  Session = Struct.new(:token, :user_id, :used)
  Project = Struct.new(:id, :name, :owner_id, :version, :deleted)
  Task = Struct.new(:id, :project_id, :title, :priority, :status, :assignee_id,
                    :internal_note, :version, :deleted)
  Comment = Struct.new(:id, :task_id, :author_id, :body)
  AuditEntry = Struct.new(:seq, :actor_id, :action, :resource, :resource_id)
  OutboxEvent = Struct.new(:seq, :name, :resource_id, :delivered)

  # Every failure path raises this. The api layer turns it into the envelope.
  class AppError < StandardError
    attr_reader :status, :code, :details

    def initialize(status, code, message, details = [])
      super(message)
      @status = status
      @code = code
      @details = details
    end
  end

  module_function

  def bad_request
    AppError.new(400, "bad_request", "the request is malformed")
  end

  def unauthorized
    AppError.new(401, "unauthorized", "authentication is required")
  end

  def invalid_credentials
    AppError.new(401, "invalid_credentials", "the username or password is wrong")
  end

  def forbidden
    AppError.new(403, "forbidden", "you may not access this resource")
  end

  def not_found
    AppError.new(404, "not_found", "the resource does not exist")
  end

  def conflict
    AppError.new(409, "conflict", "the resource already exists")
  end

  def invalid_transition
    AppError.new(409, "invalid_transition", "the status change is not allowed")
  end

  def precondition_failed
    AppError.new(412, "precondition_failed", "the resource has changed")
  end

  def precondition_required
    AppError.new(428, "precondition_required", "the If-Match header is required")
  end

  def quota_exceeded
    AppError.new(429, "quota_exceeded", "the request quota is exhausted")
  end

  def invalid(details)
    details.sort_by! { |entry| [entry[:field], entry[:message]] }
    AppError.new(422, "validation_failed", "the request body is not valid", details)
  end

  # Named fail_with, not fail, because Kernel#fail is a Ruby built-in.
  def fail_with(field, message)
    { field: field, message: message }
  end

  def compute_score(priority, status)
    base_score = priority * 10
    base_score + STATUS_BONUS[status]
  end

  def check_string(value, field_name, max_length, errors)
    if value.empty?
      errors << fail_with(field_name, "#{field_name} is required")
    elsif value.length > max_length
      errors << fail_with(field_name, "#{field_name} is too long")
    end
  end

  def check_priority(value, errors)
    return if value.is_a?(Integer) && value >= MIN_PRIORITY && value <= MAX_PRIORITY

    errors << fail_with("priority", "priority is out of range")
  end

  def check_status(value, errors)
    errors << fail_with("status", "status is not valid") unless STATUSES.include?(value)
  end

  def check_role(value, errors)
    errors << fail_with("role", "role is not valid") unless ROLES.include?(value)
  end

  def check_quota(value, errors)
    return if value.is_a?(Integer) && !value.negative?

    errors << fail_with("quota", "quota is out of range")
  end
end

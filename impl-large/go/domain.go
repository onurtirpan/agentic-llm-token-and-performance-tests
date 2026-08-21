// Task Service, large tier — domain types, constants and pure rules.
package main

import (
	"cmp"
	"encoding/json"
	"slices"
	"strings"
	"unicode/utf8"
)

const (
	maxTitleLength   = 80
	maxNameLength    = 60
	maxCommentLength = 200
	maxBulkItems     = 20
	minPriority      = 1
	maxPriority      = 5
	defaultLimit     = 20
	maxLimit         = 100
	defaultQuota     = 10000
	probeQuota       = 5
	port             = "127.0.0.1:8080"
)

var (
	roles       = []string{"admin", "user"}
	statuses    = []string{"todo", "in_progress", "done", "archived"}
	statusBonus = map[string]int{"todo": 0, "in_progress": 3, "done": 5, "archived": 0}
	transitions = [][2]string{
		{"todo", "in_progress"}, {"todo", "archived"}, {"in_progress", "todo"},
		{"in_progress", "done"}, {"done", "archived"},
	}
	projectSorts = []string{"id", "name", "taskCount"}
	taskSorts    = []string{"id", "title", "priority", "score", "status"}
	userSorts    = []string{"id", "username", "role"}
	commentSorts = []string{"id", "authorId"}
	seqSorts     = []string{"seq"}
	groupBys     = []string{"assignee", "status", "project"}
)

type User struct {
	ID       int
	Username string
	Password string
	Role     string
	Quota    int
	Version  int
	Deleted  bool
}

type Session struct {
	Token  string
	UserID int
	Used   int
}

type Project struct {
	ID      int
	Name    string
	OwnerID int
	Version int
	Deleted bool
}

type Task struct {
	ID           int
	ProjectID    int
	Title        string
	Priority     int
	Status       string
	AssigneeID   *int
	InternalNote string
	Version      int
	Deleted      bool
}

type Comment struct {
	ID       int
	TaskID   int
	AuthorID int
	Body     string
}

type AuditEntry struct {
	Seq        int
	ActorID    int
	Action     string
	Resource   string
	ResourceID int
}

type OutboxEvent struct {
	Seq        int
	Name       string
	ResourceID int
	Delivered  bool
}

// row is one serialized resource. A map keeps a conditional key honest.
type row = map[string]any

// ordered is a JSON object that keeps the order its keys were added in.
type ordered []member

type member struct {
	Key   string
	Value any
}

func (o ordered) MarshalJSON() ([]byte, error) {
	out := []byte("{")
	for index, entry := range o {
		key, err := json.Marshal(entry.Key)
		if err != nil {
			return nil, err
		}
		value, err := json.Marshal(entry.Value)
		if err != nil {
			return nil, err
		}
		if index > 0 {
			out = append(out, ',')
		}
		out = append(append(append(out, key...), ':'), value...)
	}
	return append(out, '}'), nil
}

type detail struct {
	Field   string `json:"field"`
	Message string `json:"message"`
}

// AppError is every failure path. The api layer turns it into the envelope.
type AppError struct {
	Status  int
	Code    string
	Message string
	Details []detail
}

func (e *AppError) Error() string { return e.Message }

func badRequest() *AppError {
	return &AppError{Status: 400, Code: "bad_request", Message: "the request is malformed"}
}

func unauthorized() *AppError {
	return &AppError{Status: 401, Code: "unauthorized", Message: "authentication is required"}
}

func invalidCredentials() *AppError {
	return &AppError{Status: 401, Code: "invalid_credentials",
		Message: "the username or password is wrong"}
}

func forbidden() *AppError {
	return &AppError{Status: 403, Code: "forbidden", Message: "you may not access this resource"}
}

func notFound() *AppError {
	return &AppError{Status: 404, Code: "not_found", Message: "the resource does not exist"}
}

func conflict() *AppError {
	return &AppError{Status: 409, Code: "conflict", Message: "the resource already exists"}
}

func invalidTransition() *AppError {
	return &AppError{Status: 409, Code: "invalid_transition",
		Message: "the status change is not allowed"}
}

func preconditionFailed() *AppError {
	return &AppError{Status: 412, Code: "precondition_failed",
		Message: "the resource has changed"}
}

func preconditionRequired() *AppError {
	return &AppError{Status: 428, Code: "precondition_required",
		Message: "the If-Match header is required"}
}

func quotaExceeded() *AppError {
	return &AppError{Status: 429, Code: "quota_exceeded",
		Message: "the request quota is exhausted"}
}

func invalid(details []detail) *AppError {
	slices.SortFunc(details, func(a, b detail) int {
		return cmp.Or(strings.Compare(a.Field, b.Field), strings.Compare(a.Message, b.Message))
	})
	return &AppError{Status: 422, Code: "validation_failed",
		Message: "the request body is not valid", Details: details}
}

func fail(field, message string) detail {
	return detail{Field: field, Message: message}
}

// pointer boxes a value so an absent field and a null field stay distinct.
func pointer[T any](value T) *T { return &value }

// value unwraps an optional integer, using zero so a lookup fails cleanly.
func value(number *int) int {
	if number == nil {
		return 0
	}
	return *number
}

func computeScore(priority int, status string) int {
	baseScore := priority * 10
	return baseScore + statusBonus[status]
}

func checkString(text, fieldName string, maxLength int, errors *[]detail) {
	if text == "" {
		*errors = append(*errors, fail(fieldName, fieldName+" is required"))
	} else if utf8.RuneCountInString(text) > maxLength {
		*errors = append(*errors, fail(fieldName, fieldName+" is too long"))
	}
}

func checkPriority(priority *int, errors *[]detail) {
	if priority == nil || *priority < minPriority || *priority > maxPriority {
		*errors = append(*errors, fail("priority", "priority is out of range"))
	}
}

func checkStatus(status *string, errors *[]detail) {
	if status == nil || !slices.Contains(statuses, *status) {
		*errors = append(*errors, fail("status", "status is not valid"))
	}
}

func checkRole(role *string, errors *[]detail) {
	if role == nil || !slices.Contains(roles, *role) {
		*errors = append(*errors, fail("role", "role is not valid"))
	}
}

func checkQuota(quota *int, errors *[]detail) {
	if quota == nil || *quota < 0 {
		*errors = append(*errors, fail("quota", "quota is out of range"))
	}
}

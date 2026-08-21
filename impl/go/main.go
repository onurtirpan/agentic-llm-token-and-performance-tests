// Task Service — net/http implementation.
package main

import (
	"encoding/json"
	"errors"
	"math"
	"net/http"
	"slices"
	"strconv"
)

const (
	maxTitleLength = 80
	minPriority    = 1
	maxPriority    = 5
	port           = "127.0.0.1:8080"
)

// Task is one stored record.
type Task struct {
	ID       int    `json:"id"`
	Title    string `json:"title"`
	Priority int    `json:"priority"`
	Done     bool   `json:"done"`
	Score    int    `json:"score"`
}

type taskInput struct {
	Title    string `json:"title"`
	Priority int    `json:"priority"`
	Done     bool   `json:"done"`
}

type healthResponse struct {
	Status string `json:"status"`
	Count  int    `json:"count"`
}

type listResponse struct {
	Tasks []*Task `json:"tasks"`
	Total int     `json:"total"`
}

type statsResponse struct {
	Total        int     `json:"total"`
	DoneCount    int     `json:"doneCount"`
	OpenCount    int     `json:"openCount"`
	AvgScore     float64 `json:"avgScore"`
	TopOpenTitle *string `json:"topOpenTitle"`
}

type errorResponse struct {
	Error string `json:"error"`
}

var (
	tasks  = map[int]*Task{}
	nextID = 1
)

func computeScore(priority int, done bool) int {
	baseScore := priority * 10
	if done {
		return baseScore
	}
	return baseScore + 5
}

func validate(title string, priority int) error {
	if len(title) == 0 {
		return errors.New("title is required")
	}
	if len([]rune(title)) > maxTitleLength {
		return errors.New("title is too long")
	}
	if priority < minPriority || priority > maxPriority {
		return errors.New("priority is out of range")
	}
	return nil
}

func writeJSON(w http.ResponseWriter, status int, body any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(body)
}

func fail(w http.ResponseWriter, status int, message string) {
	writeJSON(w, status, errorResponse{Error: message})
}

func parseID(raw string) (int, error) {
	return strconv.Atoi(raw)
}

func readInput(r *http.Request) (taskInput, error) {
	var input taskInput
	err := json.NewDecoder(r.Body).Decode(&input)
	return input, err
}

func sortedTasks() []*Task {
	sorted := make([]*Task, 0, len(tasks))
	for _, task := range tasks {
		sorted = append(sorted, task)
	}
	slices.SortFunc(sorted, func(a, b *Task) int { return a.ID - b.ID })
	return sorted
}

func handleHealth(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, http.StatusOK, healthResponse{Status: "ok", Count: len(tasks)})
}

func handleList(w http.ResponseWriter, r *http.Request) {
	query := r.URL.Query()
	done := query.Get("done")
	hasFilter := query.Has("done")
	if hasFilter && done != "true" && done != "false" {
		fail(w, http.StatusBadRequest, "done must be true or false")
		return
	}
	selected := []*Task{}
	for _, task := range sortedTasks() {
		if !hasFilter || task.Done == (done == "true") {
			selected = append(selected, task)
		}
	}
	slices.SortStableFunc(selected, func(a, b *Task) int {
		if a.Score != b.Score {
			return b.Score - a.Score
		}
		return a.ID - b.ID
	})
	writeJSON(w, http.StatusOK, listResponse{Tasks: selected, Total: len(selected)})
}

func handleGet(w http.ResponseWriter, r *http.Request) {
	id, err := parseID(r.PathValue("id"))
	if err != nil {
		fail(w, http.StatusBadRequest, "invalid id")
		return
	}
	task, ok := tasks[id]
	if !ok {
		fail(w, http.StatusNotFound, "task not found")
		return
	}
	writeJSON(w, http.StatusOK, task)
}

func handleCreate(w http.ResponseWriter, r *http.Request) {
	input, err := readInput(r)
	if err != nil {
		fail(w, http.StatusBadRequest, "invalid json")
		return
	}
	if err := validate(input.Title, input.Priority); err != nil {
		fail(w, http.StatusBadRequest, err.Error())
		return
	}
	task := &Task{
		ID:       nextID,
		Title:    input.Title,
		Priority: input.Priority,
		Done:     false,
		Score:    computeScore(input.Priority, false),
	}
	tasks[nextID] = task
	nextID += 1
	writeJSON(w, http.StatusCreated, task)
}

func handleUpdate(w http.ResponseWriter, r *http.Request) {
	id, err := parseID(r.PathValue("id"))
	if err != nil {
		fail(w, http.StatusBadRequest, "invalid id")
		return
	}
	task, ok := tasks[id]
	if !ok {
		fail(w, http.StatusNotFound, "task not found")
		return
	}
	input, err := readInput(r)
	if err != nil {
		fail(w, http.StatusBadRequest, "invalid json")
		return
	}
	if err := validate(input.Title, input.Priority); err != nil {
		fail(w, http.StatusBadRequest, err.Error())
		return
	}
	task.Title = input.Title
	task.Priority = input.Priority
	task.Done = input.Done
	task.Score = computeScore(input.Priority, input.Done)
	writeJSON(w, http.StatusOK, task)
}

func handleDelete(w http.ResponseWriter, r *http.Request) {
	id, err := parseID(r.PathValue("id"))
	if err != nil {
		fail(w, http.StatusBadRequest, "invalid id")
		return
	}
	if _, ok := tasks[id]; !ok {
		fail(w, http.StatusNotFound, "task not found")
		return
	}
	delete(tasks, id)
	w.WriteHeader(http.StatusNoContent)
}

func handleStats(w http.ResponseWriter, r *http.Request) {
	all := sortedTasks()
	total := len(all)
	doneCount := 0
	sumScore := 0
	var best *Task
	for _, task := range all {
		if task.Done {
			doneCount += 1
		}
		sumScore += task.Score
		if !task.Done && (best == nil || task.Priority > best.Priority) {
			best = task
		}
	}
	avgScore := 0.0
	if total > 0 {
		avgScore = math.Round(float64(sumScore)/float64(total)*100) / 100
	}
	var topOpenTitle *string
	if best != nil {
		topOpenTitle = &best.Title
	}
	writeJSON(w, http.StatusOK, statsResponse{
		Total:        total,
		DoneCount:    doneCount,
		OpenCount:    total - doneCount,
		AvgScore:     avgScore,
		TopOpenTitle: topOpenTitle,
	})
}

func handleNotFound(w http.ResponseWriter, r *http.Request) {
	fail(w, http.StatusNotFound, "not found")
}

func main() {
	mux := http.NewServeMux()
	mux.HandleFunc("GET /health", handleHealth)
	mux.HandleFunc("GET /tasks", handleList)
	mux.HandleFunc("POST /tasks", handleCreate)
	mux.HandleFunc("GET /tasks/{id}", handleGet)
	mux.HandleFunc("PUT /tasks/{id}", handleUpdate)
	mux.HandleFunc("DELETE /tasks/{id}", handleDelete)
	mux.HandleFunc("GET /stats", handleStats)
	mux.HandleFunc("/", handleNotFound)
	_ = http.ListenAndServe(port, mux)
}

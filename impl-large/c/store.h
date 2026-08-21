/* Task Service, large tier — the in-memory state and its repositories. */

#ifndef STORE_H
#define STORE_H

#include "domain.h"

typedef struct {
    char token[TOKEN_SIZE];
    char key[KEY_SIZE];
    int status;
    int etag;
    int length;
    char body[RECORD_SIZE];
} Slot;

typedef struct {
    char route[TEXT_SIZE];
    int count;
} RouteCount;

typedef struct {
    int code;
    int count;
} StatusCount;

extern User users[MAX_USERS];
extern Session sessions[MAX_SESSIONS];
extern Project projects[MAX_PROJECTS];
extern Task tasks[MAX_TASKS];
extern Comment comments[MAX_COMMENTS];
extern AuditEntry audit[MAX_AUDIT];
extern OutboxEvent outbox[MAX_OUTBOX];
extern Slot idempotency[MAX_SLOTS];
extern RouteCount byRoute[MAX_ROUTES];
extern StatusCount byStatus[MAX_CODES];

extern int userTotal;
extern int sessionTotal;
extern int projectTotal;
extern int taskTotal;
extern int commentTotal;
extern int auditTotal;
extern int outboxTotal;
extern int slotTotal;
extern int routeTotal;
extern int codeTotal;

extern int requests;
extern int nextProjectId;
extern int nextTaskId;
extern int nextCommentId;
extern int nextUserId;
extern int nextSeq;

void seed(void);
int takeSeq(void);
void record(int actorId, const char *action, const char *resource, int resourceId);
void countRequest(const char *route, int status);

User *findUser(int userId, int includeDeleted);
User *findByUsername(const char *username);
User *insertUser(const char *username, const char *password, const char *role, int quota);
int liveUsers(void);

int findSession(const char *token);
Session *insertSession(const char *token, int userId);
void removeSession(int index);

Project *findProject(int projectId, int includeDeleted);
Project *insertProject(const char *name, int ownerId);
Task *findTask(int taskId, int includeDeleted);
Task *insertTask(int projectId, const char *title, int priority, int assigneeId,
                 const char *internalNote);
Comment *findComment(int commentId);
Comment *insertComment(int taskId, int authorId, const char *body);
void dropComment(Comment *comment);

int taskCount(int projectId);
int liveProjects(void);
int liveTasks(void);
int outboxPending(void);

const Slot *findSlot(const char *token, const char *key);
void putSlot(const char *token, const char *key, int status, int etag,
             const char *body, int length);

#endif

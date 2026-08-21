/* Task Service, large tier — business rules, authorization and audit emission. */

#ifndef SERVICE_H
#define SERVICE_H

#include "domain.h"
#include "store.h"

typedef int (*Serializer)(char *out, const void *row, int isAdmin);

int serializeUser(char *out, const void *row, int isAdmin);
int serializeProject(char *out, const void *row, int isAdmin);
int serializeTask(char *out, const void *row, int isAdmin);
int serializeComment(char *out, const void *row, int isAdmin);
int serializeAudit(char *out, const void *row, int isAdmin);
int serializeOutbox(char *out, const void *row, int isAdmin);

int compareUsers(const void *left, const void *right);
int compareProjects(const void *left, const void *right);
int compareTasks(const void *left, const void *right);
int compareComments(const void *left, const void *right);
int compareSeq(const void *left, const void *right);

User *authenticate(const char *authorization, int *sessionIndex, AppError *err);
int chargeQuota(User *user, Session *session, AppError *err);
int requireAdmin(const User *user, AppError *err);
Project *reachableProject(int projectId, const User *user, int includeDeleted, AppError *err);
Task *reachableTask(int taskId, const User *user, int includeDeleted, AppError *err);
int checkIfMatch(const char *header, int hasHeader, int version, AppError *err);
int checkIncludeDeleted(const char *raw, int hasRaw, const User *user, int *out, AppError *err);

int paginate(char *out, void **rows, int total, const Page *page,
             int (*compare)(const void *, const void *), Serializer serialize, int isAdmin);

User *login(const char *username, const char *password, const char *token, AppError *err);

Project *createProject(const User *actor, const char *name, int ownerId, AppError *err);
int renameProject(const User *actor, Project *project, const char *name, AppError *err);
void deleteProject(const User *actor, Project *project);
int restoreProject(const User *actor, Project *project, AppError *err);

Task *createTask(const User *actor, const Project *project, const TaskInput *input,
                 Errors *errors, AppError *err);
int replaceTask(const User *actor, Task *task, const TaskInput *input, Errors *errors,
                AppError *err);
int moveStatus(const User *actor, Task *task, const char *status, int isString, AppError *err);
void deleteTask(const User *actor, Task *task);
int restoreTask(const User *actor, Task *task, AppError *err);

Comment *createComment(const User *actor, const Task *task, const char *body, AppError *err);
int removeComment(const User *actor, Comment *comment, AppError *err);

User *createUser(const User *actor, const UserInput *input, AppError *err);
int updateUser(const User *actor, User *target, const UserInput *input, AppError *err);
int deleteUser(const User *actor, User *target, AppError *err);

int visibleProjects(const User *user, int includeDeleted, void **out);
int visibleTasks(const User *user, int includeDeleted, void **out);
int search(const User *user, const char *query, char *out);
int workload(const User *user, const char *groupBy, char *out);
int flushOutbox(void);
int metrics(char *out);
int stats(char *out);
int checkBulkSize(int count, AppError *err);

#endif

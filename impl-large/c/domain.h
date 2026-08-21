/* Task Service, large tier — domain types, constants and pure rules. */

#ifndef DOMAIN_H
#define DOMAIN_H

#include <stddef.h>

#define MAX_TITLE_LENGTH 80
#define MAX_NAME_LENGTH 60
#define MAX_COMMENT_LENGTH 200
#define MAX_BULK_ITEMS 20
#define MIN_PRIORITY 1
#define MAX_PRIORITY 5
#define DEFAULT_LIMIT 20
#define MAX_LIMIT 100
#define DEFAULT_QUOTA 10000
#define PROBE_QUOTA 5
#define PORT 8080

#define MAX_USERS 32
#define MAX_SESSIONS 64
#define MAX_PROJECTS 128
#define MAX_TASKS 512
#define MAX_COMMENTS 256
#define MAX_AUDIT 2048
#define MAX_OUTBOX 2048
#define MAX_SLOTS 64
#define MAX_ROUTES 64
#define MAX_CODES 32

#define MAX_FIELDS 16
#define MAX_DETAILS 16
#define TEXT_SIZE 256
#define TOKEN_SIZE 40
#define KEY_SIZE 128
#define SORT_SIZE 32
#define REQUEST_SIZE 65536
#define RESPONSE_SIZE 262144
#define RECORD_SIZE 4096

#define COUNT(array) ((int)(sizeof (array) / sizeof (array)[0]))

typedef struct {
    int id;
    char username[TEXT_SIZE];
    char password[TEXT_SIZE];
    char role[8];
    int quota;
    int version;
    int deleted;
} User;

typedef struct {
    char token[TOKEN_SIZE];
    int userId;
    int used;
} Session;

typedef struct {
    int id;
    char name[TEXT_SIZE];
    int ownerId;
    int version;
    int deleted;
} Project;

typedef struct {
    int id;
    int projectId;
    char title[TEXT_SIZE];
    int priority;
    char status[16];
    int assigneeId;
    char internalNote[TEXT_SIZE];
    int version;
    int deleted;
} Task;

typedef struct {
    int id;
    int taskId;
    int authorId;
    char body[TEXT_SIZE];
} Comment;

typedef struct {
    int seq;
    int actorId;
    char action[16];
    char resource[16];
    int resourceId;
} AuditEntry;

typedef struct {
    int seq;
    char name[32];
    int resourceId;
    int delivered;
} OutboxEvent;

typedef struct {
    char field[32];
    char message[64];
} Detail;

typedef struct {
    Detail items[MAX_DETAILS];
    int count;
} Errors;

typedef struct {
    int status;
    const char *code;
    const char *message;
    Errors details;
} AppError;

typedef struct {
    int limit;
    int offset;
    char sort[SORT_SIZE];
    char order[SORT_SIZE];
} Page;

typedef struct {
    char title[TEXT_SIZE];
    int priority;
    int priorityNull;
    int assigneeId;
    char internalNote[TEXT_SIZE];
    int hasNote;
} TaskInput;

typedef struct {
    char username[TEXT_SIZE];
    char password[TEXT_SIZE];
    char role[16];
    int hasRole;
    int roleIsString;
    int quota;
    int hasQuota;
    int quotaIsInt;
} UserInput;

typedef enum {
    JSON_NULL, JSON_BOOL, JSON_INT, JSON_STRING, JSON_ARRAY, JSON_OBJECT, JSON_OTHER
} JsonKind;

typedef struct {
    char key[32];
    JsonKind kind;
    long number;
    char text[TEXT_SIZE];
    const char *raw;
} JsonField;

typedef struct {
    JsonField fields[MAX_FIELDS];
    int count;
    JsonKind kind;
} JsonBody;

typedef struct {
    int value;
    int isNull;
} IntRef;

extern const char *const STATUS_NAMES[4];
extern const char *const ROLES[2];
extern const char *const GROUP_BYS[3];
extern const char *const PROJECT_SORTS[3];
extern const char *const TASK_SORTS[5];
extern const char *const USER_SORTS[3];
extern const char *const COMMENT_SORTS[2];
extern const char *const SEQ_SORTS[1];

void copyText(char *out, size_t limit, const char *text);
int containsIgnoreCase(const char *haystack, const char *needle);
int parseWholeNumber(const char *raw, int *out);
int statusIndex(const char *status);
int computeScore(int priority, const char *status);
int allowedTransition(const char *from, const char *to);
int isMember(const char *value, const char *const *allowed, int allowedCount);

void resetError(AppError *err);
int errBadRequest(AppError *err);
int errUnauthorized(AppError *err);
int errInvalidCredentials(AppError *err);
int errForbidden(AppError *err);
int errNotFound(AppError *err);
int errConflict(AppError *err);
int errInvalidTransition(AppError *err);
int errPreconditionFailed(AppError *err);
int errPreconditionRequired(AppError *err);
int errQuotaExceeded(AppError *err);
int errInvalid(AppError *err, Errors *errors);
int errInvalidField(AppError *err, const char *field, const char *message);

void fail(Errors *errors, const char *field, const char *message);
void checkString(const char *value, const char *fieldName, size_t maxLength, Errors *errors);
void checkPriority(int value, int isNull, Errors *errors);
void checkStatus(const char *value, int isString, Errors *errors);
void checkRole(const char *value, int isString, Errors *errors);
void checkQuota(int value, int isInt, Errors *errors);

const char *skipSpace(const char *cursor);
const char *parseJsonString(const char *cursor, char *out, size_t limit);
const char *parseJsonValue(const char *cursor, JsonField *field);
const char *parseJsonObject(const char *cursor, JsonBody *body);
int parseJsonArray(const char *cursor, JsonBody *items, int capacity, int *count);
const JsonField *findField(const JsonBody *body, const char *name);
int writeJsonString(char *out, const char *text);

#endif

/*
 * Assigns — logical volume names (AmigaOS-style)
 *
 * Each assign binds a short uppercase-prefix name (Sys, Work, C, Devs,
 * ...) to an absolute VFS path.  vfs_resolve() expands "Name:rest" paths
 * through assign_expand(), so volumes are addressed by name everywhere
 * syscalls accept a path.
 */

#include <assign.h>
#include <vfs.h>
#include <kheap.h>
#include <spinlock.h>
#include <screen.h>
#include <serial.h>
#include <string.h>
#include <stddef.h>

typedef struct assign_entry {
    char name[ASSIGN_MAX_NAME];
    char path[ASSIGN_MAX_PATH];
    struct assign_entry *next;
} assign_entry_t;

static assign_entry_t *assign_list = NULL;
static spinlock_irq_t assign_lock = {0};

static assign_entry_t *find_entry_locked(const char *name)
{
    for (assign_entry_t *e = assign_list; e; e = e->next)
        if (strcmp(e->name, name) == 0)
            return e;
    return NULL;
}

int assign_set(const char *name, const char *path)
{
    if (!name || !path || !name[0] || !path[0] || path[0] != '/')
        return -1;

    int nlen = 0;
    while (name[nlen] && name[nlen] != ':' && name[nlen] != '/'
           && nlen < ASSIGN_MAX_NAME - 1)
        nlen++;
    if (name[nlen] != '\0' || nlen == 0)
        return -1;

    int plen = 0;
    while (path[plen] && plen < ASSIGN_MAX_PATH - 1)
        plen++;
    if (path[plen] != '\0')
        return -1;

    unsigned long flags;
    spin_lock_irqsave(&assign_lock, &flags);

    assign_entry_t *e = find_entry_locked(name);
    if (e) {
        strcpy(e->path, path);
        spin_unlock_irqrestore(&assign_lock, flags);
        return 0;
    }

    e = kmalloc(sizeof(assign_entry_t));
    if (!e) {
        spin_unlock_irqrestore(&assign_lock, flags);
        return -1;
    }

    strncpy(e->name, name, nlen);
    e->name[nlen] = '\0';
    strcpy(e->path, path);
    e->next = assign_list;
    assign_list = e;

    spin_unlock_irqrestore(&assign_lock, flags);
    return 0;
}

int assign_unset(const char *name)
{
    if (!name) return -1;

    unsigned long flags;
    spin_lock_irqsave(&assign_lock, &flags);

    assign_entry_t **pp = &assign_list;
    while (*pp) {
        if (strcmp((*pp)->name, name) == 0) {
            assign_entry_t *gone = *pp;
            *pp = gone->next;
            spin_unlock_irqrestore(&assign_lock, flags);
            kfree(gone);
            return 0;
        }
        pp = &(*pp)->next;
    }

    spin_unlock_irqrestore(&assign_lock, flags);
    return -1;
}

int assign_lookup(const char *name, char *out, int out_size)
{
    if (!name || !out || out_size <= 0)
        return -1;

    unsigned long flags;
    spin_lock_irqsave(&assign_lock, &flags);
    assign_entry_t *e = find_entry_locked(name);
    if (!e) {
        spin_unlock_irqrestore(&assign_lock, flags);
        return -1;
    }

    int n = 0;
    while (e->path[n] && n < out_size - 1) {
        out[n] = e->path[n];
        n++;
    }
    out[n] = '\0';
    spin_unlock_irqrestore(&assign_lock, flags);
    return 0;
}

int assign_expand(const char *path, char *out, int out_size)
{
    if (!path || !out || out_size <= 0)
        return -1;

    /* The first component must end with ':' before any '/' */
    int n = 0;
    while (path[n] && path[n] != '/' && path[n] != ':'
           && n < ASSIGN_MAX_NAME - 1)
        n++;
    if (!path[n] || path[n] != ':')
        return -1;

    char name[ASSIGN_MAX_NAME];
    for (int i = 0; i < n; i++)
        name[i] = path[i];
    name[n] = '\0';

    char target[ASSIGN_MAX_PATH];
    if (assign_lookup(name, target, sizeof target) != 0)
        return -1;

    int o = 0;
    for (int i = 0; target[i] && o < out_size - 1; i++)
        out[o++] = target[i];

    const char *rest = path + n + 1;
    while (*rest == '/')
        rest++;
    if (*rest) {
        if (o && out[o - 1] != '/' && o < out_size - 1)
            out[o++] = '/';
        while (*rest && o < out_size - 1)
            out[o++] = *rest++;
    }
    out[o] = '\0';
    return 0;
}

void assign_init(void)
{
    assign_set("Sys",  "/");
    assign_set("Ram",  "/");
    assign_set("Work", "/fat32");
    assign_set("C",    "/bin");
    assign_set("Devs", "/dev");
    serial_print("AgnusOS: Assigns: Sys: Ram: Work: C: Devs:\n");
    screen_log("OK", COLOR_LIGHT_GREEN,
               "Assigns: Sys: Ram: Work: C: Devs: disponiveis.");
}

int assign_dump(char *buf, int max)
{
    int n = 0;
    unsigned long flags;
    spin_lock_irqsave(&assign_lock, &flags);
    for (assign_entry_t *e = assign_list; e && n < max - 2; e = e->next)
        n += snprintf(buf + n, max - n, "%s: -> %s\n", e->name, e->path);
    spin_unlock_irqrestore(&assign_lock, flags);
    return n;
}

static int test_failures = 0;
static void assign_check(int cond, const char *what)
{
    if (!cond) {
        test_failures++;
        serial_print("[ASSIGN-TEST] ");
        serial_print(what);
        serial_print("\n");
        screen_log("FAIL", COLOR_LIGHT_RED, what);
    }
}

void assign_test(void)
{
    char out[ASSIGN_MAX_PATH];
    serial_print("[ASSIGN-TEST] starting...\n");

    assign_check(assign_lookup("Sys", out, sizeof out) == 0
                 && strcmp(out, "/") == 0, "ASSIGN-TEST Sys: -> /");

    assign_check(assign_lookup("C", out, sizeof out) == 0
                 && strcmp(out, "/bin") == 0, "ASSIGN-TEST C: -> /bin");

    vfs_node_t *a = vfs_resolve("C:hello");
    vfs_node_t *b = vfs_resolve("/bin/hello");
    assign_check(a != NULL && a == b, "ASSIGN-TEST C:hello resolve");

    vfs_node_t *w = vfs_resolve("Work:");
    assign_check(w != NULL, "ASSIGN-TEST Work: resolve");

    assign_check(vfs_resolve("Nosc:file") == NULL, "ASSIGN-TEST assign inexistente");

    assign_check(assign_set("Tmp", "/bin") == 0, "ASSIGN-TEST set Tmp");
    assign_check(vfs_resolve("Tmp:hello") == vfs_resolve("/bin/hello"),
                 "ASSIGN-TEST Tmp:hello resolve");
    assign_check(assign_unset("Tmp") == 0, "ASSIGN-TEST unset Tmp");
    assign_check(vfs_resolve("Tmp:hello") == NULL, "ASSIGN-TEST Tmp removido");

    if (test_failures == 0)
        serial_print("[ASSIGN-TEST] PASS\n");
    screen_log(test_failures == 0 ? "OK" : "FAIL",
               test_failures == 0 ? COLOR_LIGHT_GREEN : COLOR_LIGHT_RED,
               test_failures == 0 ? "ASSIGN-TEST PASS" : "ASSIGN-TEST falhou");
}
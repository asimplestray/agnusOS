#include <dogin.h>
#include <screen.h>
#include <serial.h>
#include <tty.h>
#include <keyboard.h>
#include <dos/dos.h>
#include <dos/path.h>
#include <task.h>
#include <pmm.h>
#include <kheap.h>
#include <assign.h>
#include <msgport.h>
#include <string.h>
#include <timer.h>
#include <rtc.h>
#include <framebuffer.h>
#include <io.h>

static char cwd_disp[ASSIGN_MAX_PATH] = "Work:";

/* helpers */
static void dogin_print(const char *s) { screen_print(s); serial_print(s); }
static void dogin_println(const char *s) { dogin_print(s); dogin_print("\n"); }

static int __attribute__((unused)) streq(const char *a, const char *b){ while(*a&&*a==*b){a++;b++;} return *a==0&&*b==0; }
static int strcaseeq(const char *a,const char *b){
    while(*a&&*b){
        char ca=*a, cb=*b;
        if(ca>='a'&&ca<='z') ca-=32;
        if(cb>='a'&&cb<='z') cb-=32;
        if(ca!=cb) return 0;
        a++;b++;
    }
    return *a==0&&*b==0;
}
static void trim(char *s){
    // ltrim
    char *p=s; while(*p==' '||*p=='\t') p++;
    if(p!=s){ int i=0; while(p[i]){s[i]=p[i]; i++;} s[i]=0; }
    // rtrim
    int n=0; while(s[n]) n++;
    while(n>0 && (s[n-1]==' '||s[n-1]=='\t'||s[n-1]=='\r'||s[n-1]=='\n')){n--; s[n]=0;}
}

static const char *skip_ws(const char *p){ while(*p==' '||*p=='\t') p++; return p; }

/* forward */
static int cmd_list(const char *args);
static int cmd_cd(const char *args);
static int cmd_makedir(const char *args);
static int cmd_delete(const char *args);
static int cmd_copy(const char *args);
static int cmd_rename(const char *args);
static int cmd_type(const char *args);
static int cmd_assign(const char *args);
static int cmd_status(const char *args);
static int cmd_avail(const char *args);
static int cmd_info(const char *args);
static int cmd_version(const char *args);
static int cmd_help(const char *args);
static int cmd_echo(const char *args);
static int cmd_run(const char *args);
static int cmd_execute(const char *args);
static int cmd_clear(const char *args);
static int cmd_date(const char *args);
static int cmd_reboot(const char *args);

typedef struct { const char *name; int (*fn)(const char*); const char *help; } cmd_t;
static cmd_t cmds[] = {
    {"List",     cmd_list,     "List [dir] - lista arquivos (alias Dir)"},
    {"Dir",      cmd_list,     "Dir [dir] - alias List"},
    {"Cd",       cmd_cd,       "Cd <dir> - muda Work: (ex: Cd Work:docs)"},
    {"MakeDir",  cmd_makedir,  "MakeDir <dir> - cria diretório"},
    {"Delete",   cmd_delete,   "Delete <file|dir> - apaga"},
    {"Copy",     cmd_copy,     "Copy <src> <dst> - copia arquivo"},
    {"Rename",   cmd_rename,   "Rename <old> <new> - renomeia"},
    {"Type",     cmd_type,     "Type <file> - mostra arquivo"},
    {"Assign",   cmd_assign,   "Assign [name [path]] - lista/cria/remove assigns (ex: Assign Work: /fat32)"},
    {"Status",   cmd_status,   "Status - lista tasks (FindTask)"},
    {"Avail",    cmd_avail,    "Avail - memória livre/total (pmm)"},
    {"Info",     cmd_info,     "Info [vol] - info do volume (ex: Info Work:)"},
    {"Version",  cmd_version,  "Version - versão do dogin/kernel"},
    {"Help",     cmd_help,     "Help [cmd] - ajuda"},
    {"Echo",     cmd_echo,     "Echo <texto> - imprime"},
    {"Run",      cmd_run,      "Run <elf> - executa ELF em Ring3 (LoadSeg)"},
    {"Execute",  cmd_execute,  "Execute <script.in> - roda .in"},
    {"Clear",    cmd_clear,    "Clear - limpa tela"},
    {"Date",     cmd_date,     "Date - data/hora RTC"},
    {"Reboot",   cmd_reboot,   "Reboot - reinicia"},
    {NULL, NULL, NULL}
};

int dogin_exec_line(const char *line){
    char buf[256];
    int i=0;
    while(line[i] && i<255){ buf[i]=line[i]; i++; }
    buf[i]=0;
    trim(buf);
    if(buf[0]==0 || buf[0]==';') return 0; // comentário Amiga ';'
    // pega comando
    char cmd[32]={0};
    const char *p=skip_ws(buf);
    int ci=0;
    while(*p && *p!=' ' && *p!='\t' && ci<31){ cmd[ci++]=*p++; }
    cmd[ci]=0;
    const char *args=skip_ws(p);
    for(int k=0; cmds[k].name; k++){
        if(strcaseeq(cmd, cmds[k].name)){
            return cmds[k].fn(args);
        }
    }
    dogin_print("dogin: comando desconhecido '");
    dogin_print(cmd);
    dogin_println("' — digite Help");
    return -1;
}

int dogin_exec_file(const char *path){
    BPTR fh = dos_open(path, MODE_OLDFILE);
    if(!fh){ dogin_print("Execute: não achei "); dogin_println(path); return -1; }

    file_info_block_t fib;
    if(dos_examine(fh, &fib)==0 && fib.fib_DirEntryType > 0){
        dos_close(fh);
        dogin_println("Execute: é diretório");
        return -1;
    }
    dos_close(fh);

    /* Re-open for reading */
    fh = dos_open(path, MODE_OLDFILE);
    if(!fh){ dogin_println("Execute: re-open falhou"); return -1; }

    dos_examine(fh, &fib);
    int32_t len = fib.fib_Size;
    if(len <= 0 || len > 8192){ dos_close(fh); dogin_println("Execute: tamanho inválido"); return -1; }

    char *data = kmalloc((uint32_t)len + 1);
    if(!data){ dos_close(fh); dogin_println("Execute: OOM"); return -1; }

    int32_t got = dos_read(fh, data, len);
    dos_close(fh);
    if(got <= 0){ kfree(data); dogin_println("Execute: leu zero bytes"); return -1; }

    data[got] = 0;

    char line[256];
    int li = 0;
    for(int32_t j = 0; j <= got; j++){
        char c = (j < got) ? data[j] : '\n';
        if(c=='\n' || c=='\r'){
            line[li] = 0;
            if(li > 0) dogin_exec_line(line);
            li = 0;
        } else if(li < 255){
            line[li++] = c;
        }
    }
    kfree(data);
    return 0;
}

/* ---- comandos ---- */
static int cmd_list(const char *args){
    const char *path = args[0]?args:"Work:";
    BPTR lock = dos_lock(path, ACCESS_READ);
    if(!lock){ dogin_print("List: não achei "); dogin_println(path); return -1; }
    file_info_block_t fib;
    if(dos_examine(lock, &fib)!=0){ dos_un_lock(lock); dogin_println("List: examine falhou"); return -1; }
    if(fib.fib_DirEntryType <= 0){ dos_un_lock(lock); dogin_println("List: não é diretório"); return -1; }
    dogin_print("List "); dogin_println(path);
    while(dos_ex_next(lock, &fib)==0){
        const char *type = (fib.fib_DirEntryType > 0) ? "<Dir>" : "     ";
        char sz[16]; itoa(fib.fib_Size, sz, 10);
        dogin_print(type); dogin_print(" "); dogin_print(sz); dogin_print(" "); dogin_println(fib.fib_FileName);
    }
    dos_un_lock(lock);
    return 0;
}
static int cmd_cd(const char *args){
    if(!args[0]){ dogin_println(cwd_disp); return 0; }
    /* Validate the path exists */
    BPTR lock = dos_lock(args, ACCESS_READ);
    if(!lock){ dogin_print("Cd: não achei "); dogin_println(args); return -1; }
    file_info_block_t fib;
    if(dos_examine(lock, &fib)!=0 || fib.fib_DirEntryType <= 0){
        dos_un_lock(lock);
        dogin_println("Cd: não é diretório");
        return -1;
    }
    dos_un_lock(lock);

    /* Update Work: assign and display */
    if(args[0] && args[1]==':'){
        char expanded[ASSIGN_MAX_PATH];
        if(assign_expand(args, expanded, sizeof expanded)==0){
            assign_set("Work", expanded);
            strncpy(cwd_disp, args, sizeof cwd_disp-1);
        } else {
            strncpy(cwd_disp, args, sizeof cwd_disp-1);
        }
    } else {
        char cur[ASSIGN_MAX_PATH];
        if(assign_lookup("Work", cur, sizeof cur)!=0) strcpy(cur, "/fat32");
        char nxt[ASSIGN_MAX_PATH];
        dos_add_part(cur, args, nxt, sizeof nxt);
        assign_set("Work", nxt);
        strcpy(cwd_disp, "Work:");
        if(args[0]!='/'){
            char *dst = cwd_disp;
            while(*dst) dst++;
            int rem = (int)sizeof(cwd_disp) - (int)(dst - cwd_disp) - 1;
            while(*args && rem > 0){ *dst++ = *args++; rem--; }
            *dst = 0;
        }
        else {
            char tmp[ASSIGN_MAX_PATH];
            assign_expand("Work:", tmp, sizeof tmp);
            strcpy(cwd_disp, tmp);
        }
    }
    dogin_print("Cd para "); dogin_println(cwd_disp);
    return 0;
}
static int cmd_makedir(const char *args){
    if(!args[0]){ dogin_println("MakeDir: falta nome"); return -1; }
    int32_t rc = dos_create_dir(args);
    if(rc==0) dogin_println("MakeDir: ok");
    else dogin_println("MakeDir: falhou (já existe?)");
    return rc;
}
static int cmd_delete(const char *args){
    if(!args[0]){ dogin_println("Delete: falta nome"); return -1; }
    int32_t rc = dos_delete_file(args);
    if(rc==0) dogin_println("Delete: ok");
    else dogin_println("Delete: falhou");
    return rc;
}
static int cmd_copy(const char *args){
    char src[ASSIGN_MAX_PATH]={0}, dst[ASSIGN_MAX_PATH]={0};
    int i=0,j=0;
    const char *p=skip_ws(args);
    while(*p && *p!=' ' && *p!='\t' && i<ASSIGN_MAX_PATH-1) src[i++]=*p++;
    src[i]=0; p=skip_ws(p);
    while(*p && j<ASSIGN_MAX_PATH-1) dst[j++]=*p++;
    dst[j]=0; trim(dst);
    if(!src[0]||!dst[0]){ dogin_println("Copy: uso Copy <src> <dst>"); return -1; }

    BPTR sfh = dos_open(src, MODE_OLDFILE);
    if(!sfh){ dogin_print("Copy: src não achei "); dogin_println(src); return -1; }

    file_info_block_t sfib;
    if(dos_examine(sfh, &sfib)==0 && sfib.fib_DirEntryType > 0){
        dos_close(sfh);
        dogin_println("Copy: src é dir (Use MakeDir)");
        return -1;
    }

    /* Re-open after examine consumed position */
    dos_close(sfh);
    sfh = dos_open(src, MODE_OLDFILE);
    if(!sfh){ dogin_println("Copy: re-open falhou"); return -1; }

    file_info_block_t sfib2;
    dos_examine(sfh, &sfib2);
    uint32_t len = (uint32_t)sfib2.fib_Size;
    if(len == 0){ dos_close(sfh); dogin_println("Copy: src vazio"); return 0; }

    uint8_t *buf = kmalloc(len ? len : 1);
    if(!buf){ dos_close(sfh); dogin_println("Copy: OOM"); return -1; }

    int32_t got = dos_read(sfh, (void*)buf, (int32_t)len);
    dos_close(sfh);

    if(got <= 0){ kfree(buf); dogin_println("Copy: leu zero bytes"); return -1; }

    BPTR dfh = dos_open(dst, MODE_OLDFILE);
    if(dfh){
        dos_write(dfh, buf, got);
        dos_close(dfh);
        dogin_println("Copy: sobrescrito");
    } else {
        dogin_print("Copy: dst não existe, criando "); dogin_println(dst);
        /* TODO: MODE_NEWFILE + ramfs file creation */
        dogin_println("Copy: criação de arquivo novo ainda não implementada — use Type/Copy sobre arquivo existente");
        kfree(buf);
        return -1;
    }

    kfree(buf);
    return 0;
}
static int cmd_rename(const char *args){
    char oldp[ASSIGN_MAX_PATH]={0}, newp[ASSIGN_MAX_PATH]={0};
    int i=0,j=0;
    const char *p=skip_ws(args);
    while(*p && *p!=' ' && *p!='\t' && i<ASSIGN_MAX_PATH-1) oldp[i++]=*p++;
    oldp[i]=0; p=skip_ws(p);
    while(*p && j<ASSIGN_MAX_PATH-1) newp[j++]=*p++;
    newp[j]=0; trim(newp);
    if(!oldp[0]||!newp[0]){ dogin_println("Rename: uso Rename <old> <new>"); return -1;}
    int32_t rc = dos_rename(oldp, newp);
    if(rc==0) dogin_println("Rename: ok"); else dogin_println("Rename: falhou");
    return rc;
}
static int cmd_type(const char *args){
    if(!args[0]){ dogin_println("Type: falta arquivo"); return -1; }
    BPTR fh = dos_open(args, MODE_OLDFILE);
    if(!fh){ dogin_print("Type: não achei "); dogin_println(args); return -1; }
    file_info_block_t fib;
    if(dos_examine(fh, &fib)==0 && fib.fib_DirEntryType > 0){
        dos_close(fh);
        dogin_println("Type: é diretório, use List");
        return -1;
    }
    /* Re-open — dos_examine consumed the handle position */
    dos_close(fh);
    fh = dos_open(args, MODE_OLDFILE);
    if(!fh){ dogin_print("Type: re-open falhou "); dogin_println(args); return -1; }
    char buf[8193];
    int32_t got = dos_read(fh, buf, 8192);
    dos_close(fh);
    if(got <= 0){ dogin_println("Type: arquivo vazio"); return 0; }
    buf[got] = 0;
    dogin_print(buf);
    if(got > 0 && buf[got-1]!='\n') dogin_print("\n");
    return 0;
}
static int cmd_assign(const char *args){
    if(!args[0]){
        char dump[512];
        int n=assign_dump(dump, sizeof dump);
        dump[n]=0;
        dogin_print(dump);
        return 0;
    }
    // Assign name [path]  ou Assign name: path
    char name[ASSIGN_MAX_NAME]={0}, path[ASSIGN_MAX_PATH]={0};
    const char *p=skip_ws(args);
    int ni=0;
    while(*p && *p!=' ' && *p!='\t' && *p!=':' && ni<ASSIGN_MAX_NAME-1) name[ni++]=*p++;
    name[ni]=0;
    if(*p==':') p++;
    p=skip_ws(p);
    if(!p[0]){
        // get
        char out[ASSIGN_MAX_PATH];
        if(assign_lookup(name, out, sizeof out)==0){ dogin_print(name); dogin_print(": -> "); dogin_println(out); }
        else dogin_println("Assign: não achei");
        return 0;
    }
    // set
    int pi=0;
    while(*p && pi<ASSIGN_MAX_PATH-1) path[pi++]=*p++;
    path[pi]=0; trim(path);
    // path pode ser Work:docs — expande?
    if(assign_set(name, path)==0) dogin_println("Assign: ok");
    else dogin_println("Assign: falhou (use /abs)");
    return 0;
}
static int cmd_status(const char *args){
    (void)args;
    dogin_println("Status: tasks (FindTask)");
    task_struct_t *t=task_list;
    if(!t){ dogin_println("  (nenhuma)"); return 0; }
    int count=0;
    do{
        char pidb[16];
        itoa(t->pid, pidb, 10);
        const char *st="RUN";
        if(t->state==TASK_STATE_INTERRUPTIBLE) st="WAIT";
        else if(t->state==TASK_STATE_ZOMBIE) st="ZOMBIE";
        else if(t->state==TASK_STATE_STOPPED) st="STOP";
        dogin_print("  PID "); dogin_print(pidb); dogin_print(" "); dogin_print(st);
        if(t==current) dogin_print(" <current>");
        dogin_print("\n");
        t=t->next;
        count++;
        if(count>32) break;
    } while(t!=task_list);
    return 0;
}
static int cmd_avail(const char *args){
    (void)args;
    uint64_t total=pmm_get_total_memory();
    uint64_t free=pmm_get_free_memory();
    uint64_t used=total-free;
    char b1[32],b2[32],b3[32];
    itoa(total/1024, b1, 10);
    itoa(free/1024, b2, 10);
    itoa(used/1024, b3, 10);
    dogin_print("Avail: Total "); dogin_print(b1); dogin_print("K  Free "); dogin_print(b2); dogin_print("K  Used "); dogin_print(b3); dogin_println("K");
    // heap
    dogin_println("  (heap: kmalloc/kfree)");
    return 0;
}
static int cmd_info(const char *args){
    const char *vol = args[0]?args:"Work:";
    BPTR lock = dos_lock(vol, ACCESS_READ);
    if(!lock){ dogin_print("Info: não achei "); dogin_println(vol); return -1; }
    file_info_block_t fib;
    if(dos_examine(lock, &fib)!=0){ dos_un_lock(lock); dogin_println("Info: examine falhou"); return -1; }

    char expanded[ASSIGN_MAX_PATH];
    const char *path=vol;
    if(vol[0] && vol[1]==':'){
        if(assign_expand(vol, expanded, sizeof expanded)==0) path=expanded;
    }
    dogin_print("Info "); dogin_println(vol);
    dogin_print("  Path: "); dogin_println(path);
    dogin_print("  Tipo: "); dogin_println((fib.fib_DirEntryType > 0)?"Dir":"File");
    if(fib.fib_DirEntryType > 0){
        int cnt=0;
        while(dos_ex_next(lock, &fib)==0) cnt++;
        char cb[16]; itoa(cnt, cb, 10);
        dogin_print("  Entries: "); dogin_println(cb);
    } else {
        char sz[16]; itoa(fib.fib_Size, sz, 10);
        dogin_print("  Size: "); dogin_print(sz); dogin_println(" bytes");
    }
    dos_un_lock(lock);
    return 0;
}
static int cmd_version(const char *args){
    (void)args;
    dogin_println("dogin 0.1 — AmigaDOS-like shell para ApolloOS");
    dogin_println("  kernel v0.2-Alpha  exec 0.1  dos 0.1  intuition 0.1");
    dogin_println("  AOS_53 traps  Assign/MsgPort  Limine BIOS+UEFI");
    dogin_println("  .in scripts com ';' comentário");
    return 0;
}
static int cmd_help(const char *args){
    if(args[0]){
        for(int i=0;cmds[i].name;i++) if(strcaseeq(args, cmds[i].name)){ dogin_print(cmds[i].name); dogin_print(" - "); dogin_println(cmds[i].help); return 0; }
        dogin_print("Help: não achei "); dogin_println(args);
        return -1;
    }
    dogin_println("Comandos Amiga-like:");
    for(int i=0;cmds[i].name;i++){ dogin_print("  "); dogin_print(cmds[i].name); dogin_print(" - "); dogin_println(cmds[i].help); }
    dogin_println("  ; comentário  e  Work:Assigns  e  .in scripts");
    return 0;
}
static int cmd_echo(const char *args){ dogin_println(args); return 0; }
static int cmd_run(const char *args){
    if(!args[0]){ dogin_println("Run: falta ELF (ex: Run C:hello)"); return -1; }
    BPTR fh = dos_open(args, MODE_OLDFILE);
    if(!fh){ dogin_print("Run: não achei "); dogin_println(args); return -1; }
    dos_close(fh);
    dogin_print("Run: LoadSeg "); dogin_println(args);
    dogin_println("Run: task_create_user not implemented yet");
    return -1;
}
static int cmd_execute(const char *args){
    if(!args[0]){ dogin_println("Execute: falta .in (ex: Execute Work:setup.in)"); return -1; }
    return dogin_exec_file(args);
}
static int cmd_clear(const char *args){
    (void)args;
    screen_clear(COLOR_BLACK);
    return 0;
}
static int cmd_date(const char *args){
    (void)args;
    uint64_t secs = rtc_get_epoch_seconds();
    uint64_t days = secs / 86400;
    uint64_t rem = secs % 86400;
    uint64_t h = rem / 3600;
    uint64_t m = (rem % 3600) / 60;
    uint64_t s = rem % 60;
    dogin_print("Date: epoch ");
    char b[32];
    itoa(secs, b, 10); dogin_print(b);
    dogin_print(" (");
    itoa(days, b, 10); dogin_print(b); dogin_print(" dias) ");
    itoa(h, b, 10); dogin_print(b); dogin_print(":");
    itoa(m, b, 10); dogin_print(b); dogin_print(":");
    itoa(s, b, 10); dogin_println(b);
    return 0;
}
static int cmd_reboot(const char *args){
    (void)args;
    dogin_println("Reboot: reiniciando via 0x64...");
    outb(0x64, 0xFE);
    while(1) __asm__ volatile("hlt");
    return 0;
}

void dogin_init(void){
    dogin_println("dogin: Workbench-like shell 0.1 (AmigaDOS)");
    dogin_println("  digite Help para comandos, ; para comentário, Work: para assigns");
}

void dogin_main(void){
    char line[256];
    dogin_init();
    // prompt inicial
    while(1){
        dogin_print(cwd_disp);
        dogin_print("> ");
        // ler linha via TTY (canon)
        int len=0;
        // usa tty_read bloqueante
        if(console_tty){
            int r = tty_read(console_tty, line, sizeof line-1);
            if(r>0){
                line[r]=0;
                // remove \n
                for(int i=0;i<r;i++) if(line[i]=='\n' || line[i]=='\r'){ line[i]=0; break; }
                dogin_exec_line(line);
            } else if(r==0){
                continue;
            } else {
                // EINTR ou erro
                continue;
            }
        } else {
            // fallback polling keyboard buffer direto (sem TTY)
            // espera tecla
            len=0;
            while(1){
                char c = keyboard_pop_char();
                if(c){
                    if(c=='\n' || c=='\r'){
                        line[len]=0;
                        screen_print("\n");
                        break;
                    } else if(c=='\b' || c==127){
                        if(len>0){ len--; screen_print("\b \b"); }
                    } else if(len< (int)sizeof line-1 && c>=32 && c<127){
                        line[len++]=c;
                        screen_putc(c);
                    }
                } else {
                    __asm__ volatile("hlt");
                }
            }
            line[len]=0;
            dogin_exec_line(line);
        }
        // resched para não travar
        // kworker já drena WQ, mas dogin precisa yield
        // usa hlt + schedule via timer
        __asm__ volatile("hlt");
    }
}

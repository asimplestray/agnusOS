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

/* ================================================================== */
/* Control flow state — If/Then/Else/EndIf + While/EndWhile nesting    */
/* ================================================================== */
#define IF_MAX_DEPTH 16
#define LOOP_MAX_DEPTH 8

static int if_depth = 0;
static int skip_depth = 0;
static int if_was_true[IF_MAX_DEPTH];

/* Script loop support — set by dogin_exec_file, read by handlers */
static char **script_lines = NULL;
static int script_line_count = 0;
static int script_line_idx = 0;

/* While/Repeat loop stack */
typedef struct {
    int line_idx;   /* line to loop back to */
    int is_repeat;  /* 1=Repeat/Until, 0=While/EndWhile */
} loop_entry_t;
static loop_entry_t loop_stack[LOOP_MAX_DEPTH];
static int loop_sp = 0;

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

/* Case-insensitive word prefix check: does `str` start with `word` + whitespace/end? */
static int starts_with_word_ci(const char *str, const char *word){
    int len = 0;
    while(word[len]) len++;
    for(int i = 0; i < len; i++){
        char cs = str[i], cw = word[i];
        if(cs >= 'a' && cs <= 'z') cs -= 32;
        if(cw >= 'a' && cw <= 'z') cw -= 32;
        if(cs != cw) return 0;
    }
    char c = str[len];
    return c == 0 || c == ' ' || c == '\t';
}

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
static int cmd_which(const char *args);
static int cmd_path(const char *args);
static int cmd_set(const char *args);
static int cmd_unset(const char *args);

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
    {"Which",    cmd_which,    "Which <name> - procura em C:"},
    {"Path",     cmd_path,     "Path [dir] - mostra/adiciona path de busca"},
    {"Set",      cmd_set,      "Set <var> <valor> - define variável de ambiente"},
    {"Unset",    cmd_unset,    "Unset <var> - remove variável"},
    {NULL, NULL, NULL}
};

/* ================================================================== */
/* Control flow handlers — If/Then/Else/EndIf/While/EndWhile/Repeat/Until */
/* ================================================================== */

/* Find "Then" keyword in args, returns pointer to it or NULL */
static const char *find_then(const char *s){
    const char *p = skip_ws(s);
    while(*p){
        if((p==s || *(p-1)==' ' || *(p-1)=='\t') &&
           (p[0]=='T'||p[0]=='t') && (p[1]=='h'||p[1]=='H') &&
           (p[2]=='e'||p[2]=='e') && (p[3]=='n'||p[3]=='n')){
            char after = p[4];
            if(after==0 || after==' ' || after=='\t') return p;
        }
        p++;
    }
    return NULL;
}

/* Find "Do" keyword in args (for While) */
static const char *find_do(const char *s){
    const char *p = skip_ws(s);
    while(*p){
        if((p==s || *(p-1)==' ' || *(p-1)=='\t') &&
           (p[0]=='D'||p[0]=='d') && (p[1]=='o'||p[1]=='o')){
            char after = p[2];
            if(after==0 || after==' ' || after=='\t') return p;
        }
        p++;
    }
    return NULL;
}

static int handle_if(const char *line_after_if){
    const char *p = skip_ws(line_after_if);
    const char *then = find_then(p);
    if(!then){ dogin_println("If: falta Then"); return -1; }

    /* extract condition */
    int clen = then - p;
    if(clen > 255) clen = 255;
    char condition[256];
    memcpy(condition, p, clen);
    condition[clen] = 0;
    trim(condition);

    int result = 0;
    if(condition[0]) result = dogin_exec_line(condition);

    const char *after = skip_ws(then + 4);
    if(after[0] == 0){
        /* block form: If <cond> Then\n */
        if(if_depth >= IF_MAX_DEPTH){ dogin_println("If: muito profundo"); return -1; }
        if_was_true[if_depth] = (result == 0);
        if_depth++;
        if(result != 0) skip_depth = 1;
    } else {
        /* single-line: If <cond> Then <action> */
        if(result == 0) return dogin_exec_line(after);
    }
    return 0;
}

static int handle_else(void){
    if(if_depth == 0){ dogin_println("Else: sem If"); return -1; }
    if(skip_depth == 0){
        /* were executing Then block → skip Else block */
        skip_depth = 1;
    } else if(skip_depth == 1){
        /* were skipping Then block → execute Else block */
        skip_depth = 0;
    }
    return 0;
}

static int handle_endif(void){
    if(if_depth == 0){ dogin_println("EndIf: sem If"); return -1; }
    if_depth--;
    if(skip_depth > 0) skip_depth--;
    return 0;
}

static int handle_while(const char *line_after_while){
    if(loop_sp >= LOOP_MAX_DEPTH){ dogin_println("While: muito profundo"); return -1; }
    const char *p = skip_ws(line_after_while);
    const char *do_kw = find_do(p);
    if(!do_kw){ dogin_println("While: falta Do"); return -1; }

    int clen = do_kw - p;
    if(clen > 255) clen = 255;
    char condition[256];
    memcpy(condition, p, clen);
    condition[clen] = 0;
    trim(condition);

    int result = 0;
    if(condition[0]) result = dogin_exec_line(condition);

    if(result != 0){
        /* condition false → skip to EndWhile */
        skip_depth = 1;
    }
    /* store loop start for EndWhile */
    loop_stack[loop_sp].line_idx = script_line_idx;
    loop_stack[loop_sp].is_repeat = 0;
    loop_sp++;
    return 0;
}

static int handle_endwhile(void){
    if(loop_sp == 0 || loop_stack[loop_sp-1].is_repeat){
        dogin_println("EndWhile: sem While"); return -1;
    }
    loop_sp--;
    if(skip_depth > 0){
        /* we were skipping, block is done */
        return 0;
    }
    /* loop back — adjust script_line_idx so next exec will re-run the While line */
    script_line_idx = loop_stack[loop_sp].line_idx - 1;
    return 0;
}

static int handle_repeat(void){
    if(loop_sp >= LOOP_MAX_DEPTH){ dogin_println("Repeat: muito profundo"); return -1; }
    loop_stack[loop_sp].line_idx = script_line_idx + 1;
    loop_stack[loop_sp].is_repeat = 1;
    loop_sp++;
    return 0;
}

static int handle_until(const char *line_after_until){
    if(loop_sp == 0 || !loop_stack[loop_sp-1].is_repeat){
        dogin_println("Until: sem Repeat"); return -1;
    }
    const char *p = skip_ws(line_after_until);
    int result = 0;
    if(p[0]) result = dogin_exec_line(p);

    loop_sp--;
    if(result != 0){
        /* condition true (non-zero return) → exit loop */
        return 0;
    }
    /* condition false → loop back to Repeat */
    script_line_idx = loop_stack[loop_sp].line_idx - 1;
    return 0;
}

int dogin_exec_line(const char *line){
    char buf[256];
    int i=0;
    while(line[i] && i<255){ buf[i]=line[i]; i++; }
    buf[i]=0;
    trim(buf);
    if(buf[0]==0 || buf[0]==';') return 0; // comentário Amiga ';'

    const char *p = skip_ws(buf);

    /* --- Control flow: skipping block (skip_depth > 0) --- */
    if(skip_depth > 0){
        /* nested block starts add to skip depth */
        if(starts_with_word_ci(p, "If") || starts_with_word_ci(p, "While") ||
           starts_with_word_ci(p, "Repeat")){
            skip_depth++;
            return 0;
        }
        /* block ends decrement skip depth */
        if(starts_with_word_ci(p, "EndIf") || starts_with_word_ci(p, "EndWhile") ||
           starts_with_word_ci(p, "EndFor")){
            if(skip_depth > 0) skip_depth--;
            return 0;
        }
        /* Else at our level: toggle */
        if(starts_with_word_ci(p, "Else") && skip_depth == 1){
            skip_depth = 0;
            return 0;
        }
        return 0; /* skip everything else */
    }

    /* --- Control flow keywords (not skipping) --- */
    if(starts_with_word_ci(p, "If")){
        return handle_if(p + 2);
    }
    if(starts_with_word_ci(p, "Else")){
        return handle_else();
    }
    if(starts_with_word_ci(p, "EndIf")){
        return handle_endif();
    }
    if(starts_with_word_ci(p, "While")){
        return handle_while(p + 5);
    }
    if(starts_with_word_ci(p, "EndWhile")){
        return handle_endwhile();
    }
    if(starts_with_word_ci(p, "Repeat")){
        return handle_repeat();
    }
    if(starts_with_word_ci(p, "Until")){
        return handle_until(p + 5);
    }

    /* --- Normal command dispatch --- */
    char cmd[32]={0};
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

    /* Split into lines for loop support (While/EndWhile, Repeat/Until) */
    #define MAX_SCRIPT_LINES 256
    char *lines[MAX_SCRIPT_LINES];
    int line_count = 0;
    char line[256];
    int li = 0;
    for(int32_t j = 0; j <= got; j++){
        char c = (j < got) ? data[j] : '\n';
        if(c=='\n' || c=='\r'){
            line[li] = 0;
            if(li > 0 && line_count < MAX_SCRIPT_LINES){
                lines[line_count] = kmalloc(li + 1);
                if(lines[line_count]){
                    memcpy(lines[line_count], line, li + 1);
                    line_count++;
                }
            }
            li = 0;
        } else if(li < 255){
            line[li++] = c;
        }
    }
    kfree(data);

    /* Save script context, execute line-by-line with loop support */
    char **prev_lines = script_lines;
    int prev_count = script_line_count;
    int prev_idx = script_line_idx;

    script_lines = lines;
    script_line_count = line_count;
    script_line_idx = 0;

    while(script_line_idx < script_line_count){
        dogin_exec_line(lines[script_line_idx]);
        script_line_idx++;
        /* Guard: max iterations to prevent infinite loops */
        if(script_line_idx > line_count * 1000){
            dogin_println("Execute: loop infinito (max 1000x)");
            break;
        }
    }

    /* Restore script context */
    script_lines = prev_lines;
    script_line_count = prev_count;
    script_line_idx = prev_idx;

    /* Free allocated lines */
    for(int i = 0; i < line_count; i++){
        if(lines[i]) kfree(lines[i]);
    }

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
        if(t->state==TASK_STATE_WAITING) st="WAIT";
        else if(t->state==TASK_STATE_SUSPENDED) st="SUSP";
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
    dogin_println("  Estruturas de controle:");
    dogin_println("    If <cmd> Then ... [Else ...] EndIf");
    dogin_println("    While <cmd> Do ... EndWhile   (scripts .in)");
    dogin_println("    Repeat ... Until <cmd>         (scripts .in)");
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

/* ------------------------------------------------------------------ */
/* Which — find command in C: assign                                   */
/* ------------------------------------------------------------------ */
static int cmd_which(const char *args){
    if(!args[0]){ dogin_println("Which: falta nome"); return -1; }
    char path[ASSIGN_MAX_PATH];
    /* Try C: prefix */
    strcpy(path, "C:");
    int i = 0;
    while(args[i] && i < (int)(ASSIGN_MAX_PATH - 4)){ path[2 + i] = args[i]; i++; }
    path[2 + i] = 0;
    BPTR fh = dos_open(path, MODE_OLDFILE);
    if(fh){
        dos_close(fh);
        dogin_print("Which: "); dogin_print(path); dogin_println(" (encontrado)");
        return 0;
    }
    dogin_print("Which: não achei '"); dogin_print(args); dogin_println("' em C:");
    return -1;
}

/* ------------------------------------------------------------------ */
/* Path — show/modify search path                                      */
/* ------------------------------------------------------------------ */
static char search_path[ASSIGN_MAX_PATH] = "Work:bin";

static int cmd_path(const char *args){
    if(!args[0]){
        dogin_print("Path: "); dogin_println(search_path);
        return 0;
    }
    strncpy(search_path, args, sizeof(search_path) - 1);
    search_path[sizeof(search_path) - 1] = 0;
    dogin_print("Path: "); dogin_println(search_path);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Set / Unset — environment variables (simple key=value store)        */
/* ------------------------------------------------------------------ */
#define ENV_MAX 16
#define ENV_KEYLEN 32
#define ENV_VALLEN 64
typedef struct { char key[ENV_KEYLEN]; char val[ENV_VALLEN]; } env_var_t;
static env_var_t env_vars[ENV_MAX];
static int env_count = 0;

static int cmd_set(const char *args){
    if(!args[0]){ dogin_println("Set: falta variável"); return -1; }
    char key[ENV_KEYLEN] = {0};
    const char *p = args;
    int ki = 0;
    while(*p && *p != '=' && ki < ENV_KEYLEN - 1) key[ki++] = *p++;
    key[ki] = 0;
    const char *val = "";
    if(*p == '=') val = p + 1;
    /* Find or create */
    for(int i = 0; i < env_count; i++){
        if(strcmp(env_vars[i].key, key) == 0){
            strncpy(env_vars[i].val, val, ENV_VALLEN - 1);
            env_vars[i].val[ENV_VALLEN - 1] = 0;
            dogin_print(key); dogin_print("="); dogin_println(val);
            return 0;
        }
    }
    if(env_count < ENV_MAX){
        strncpy(env_vars[env_count].key, key, ENV_KEYLEN - 1);
        strncpy(env_vars[env_count].val, val, ENV_VALLEN - 1);
        env_count++;
        dogin_print(key); dogin_print("="); dogin_println(val);
    } else {
        dogin_println("Set: muitas variáveis");
        return -1;
    }
    return 0;
}

static int cmd_unset(const char *args){
    if(!args[0]){ dogin_println("Unset: falta variável"); return -1; }
    for(int i = 0; i < env_count; i++){
        if(strcmp(env_vars[i].key, args) == 0){
            /* Shift down */
            for(int j = i; j < env_count - 1; j++)
                env_vars[j] = env_vars[j + 1];
            env_count--;
            dogin_println("Unset: ok");
            return 0;
        }
    }
    dogin_println("Unset: não achei");
    return -1;
}

void dogin_init(void){
    dogin_println("dogin: Workbench-like shell 0.1 (AmigaDOS)");
    dogin_println("  digite Help para comandos, ; para comentário, Work: para assigns");
    dogin_println("  If/Then/Else/EndIf, While/EndWhile, Repeat/Until");
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

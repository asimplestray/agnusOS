#ifndef DOGIN_H
#define DOGIN_H

/* dogin — Workbench-like shell AmigaOS (kernel thread, depois /bin/dogin Ring3)
 * Prompt: "dogin:Work> " (Work: vem de Assign Work: -> /fat32)
 * Scripts: .in (Amiga Execute) — linha a linha, ';' comentário, Assign/echo/run
 */

void dogin_init(void);
void dogin_main(void);  // kthread entry
int dogin_exec_line(const char *line);
int dogin_exec_file(const char *path); // .in

#endif

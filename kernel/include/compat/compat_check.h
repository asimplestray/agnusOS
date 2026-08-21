/*
 * compat/compat_check.h — validador da camada de compatibilidade.
 *
 * Fase 2 Dev 4 — entrega: prova que os headers compat/linux_*.h compilam
 * e se comportam conforme a convenção (ver GPU_PORTING_STRATEGY.md §3).
 */

#ifndef _COMPAT_COMPAT_CHECK_H_
#define _COMPAT_COMPAT_CHECK_H_

/* Roda os checks de runtime da compat layer. 0 = OK, -N = N falhas. */
int compat_layer_test(void);

#endif /* _COMPAT_COMPAT_CHECK_H_ */

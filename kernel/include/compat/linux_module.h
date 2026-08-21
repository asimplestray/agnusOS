/*
 * compat/linux_module.h — macros de módulo estilo Linux (no-ops).
 *
 * Fase 2 Dev 4 — Camada de Compatibilidade.
 *
 * O ApolloOS não tem carregamento de módulos: código em port usa estas
 * macros para compilar sem patch. (Substitui os #define locais que o
 * apollo_drv.c carregava manualmente.)
 */

#ifndef _COMPAT_LINUX_MODULE_H_
#define _COMPAT_LINUX_MODULE_H_

#include <compat/linux_types.h>

/* Usados em file scope no código-fonte Linux: devem expandir para nada
 * (mesma convenção que o apollo_drv.c usava antes da compat layer). */
#define module_init(fn)
#define module_exit(fn)

#define MODULE_LICENSE(lic)
#define MODULE_AUTHOR(auth)
#define MODULE_DESCRIPTION(desc)
#define MODULE_VERSION(ver)
#define MODULE_ALIAS(alias)
#define MODULE_DEVICE_TABLE(type, table)
#define MODULE_SUPPORTED_DEVICE(dev)

#define __init
#define __exit
#define __devinit
#define __devexit
#define __read_mostly
#define __aligned(x) __attribute__((aligned(x)))

#endif /* _COMPAT_LINUX_MODULE_H_ */

#ifndef __DIAGNOSTICS_RT_CONFIG_H__
#define __DIAGNOSTICS_RT_CONFIG_H__

#include "ep-rt-config.h"

#ifdef EP_INLINE_GETTER_SETTER
#define DS_INLINE_GETTER_SETTER
#endif

#ifdef EP_INCLUDE_SOURCE_FILES
#define DS_INCLUDE_SOURCE_FILES
#endif

#ifdef ENABLE_PERFTRACING_PAL_TCP
#define DS_IPC_PAL_TCP
#else
#ifndef HOST_WIN32
#define DS_IPC_PAL_UDS
#else
#define DS_IPC_PAL_NAMEDPIPES
#endif
#endif

#ifdef DISABLE_PERFTRACING_CONNECT_PORTS
#define DS_IPC_DISABLE_CONNECT_PORTS
#endif

#ifdef DISABLE_PERFTRACING_LISTEN_PORTS
#define DS_IPC_DISABLE_LISTEN_PORTS
#define DS_IPC_DISABLE_DEFAULT_LISTEN_PORT
#endif

#ifdef DISABLE_PERFTRACING_DEFAULT_LISTEN_PORT
#define DS_IPC_DISABLE_DEFAULT_LISTEN_PORT
#endif

#endif /* __DIAGNOSTICS_RT_CONFIG_H__ */

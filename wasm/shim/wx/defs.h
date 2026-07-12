/*
 * Minimal wx/defs.h shim — satisfies includes/private/wx-ui/wx-utils.h when
 * building the headless wasm core.  Only the few typedefs/enums the header
 * itself references are provided; no wxWidgets code is compiled or linked.
 */
#ifndef _PCEM_WASM_WX_DEFS_SHIM_H_
#define _PCEM_WASM_WX_DEFS_SHIM_H_

#include <stdint.h>

typedef intptr_t wxIntPtr;
typedef int32_t wxInt32;

enum wxItemKind { wxITEM_SEPARATOR = -1, wxITEM_NORMAL, wxITEM_CHECK, wxITEM_RADIO, wxITEM_DROPDOWN, wxITEM_MAX };

#define WX_MB_OK 1

#endif

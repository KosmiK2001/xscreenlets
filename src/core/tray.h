#ifndef XS_TRAY_H
#define XS_TRAY_H

#include "xs_api.h"

void xs_tray_init(void);
void xs_tray_add_plugin(XsPlugin *p);
void xs_tray_remove_plugin(XsPlugin *p);
void xs_tray_set_plugin_visibility(XsPlugin *p, gboolean visible);
void xs_tray_rebuild(void);
void xs_tray_shutdown(void);

#endif /* XS_TRAY_H */
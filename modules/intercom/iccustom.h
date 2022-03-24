/**
 * @file iccustom.h Custom Intercom call interface
 *
 * Copyright (C) 2022 Commend.com - c.spielberger@commend.com
 */

int iccustom_init(void);
void iccustom_close(void);
bool ic_is_custom(const struct pl *val);
enum sdp_dir iccustom_dir(const struct pl *val);
bool iccustom_lookup(struct le *le, void *arg);
bool iccustom_allowed(const struct pl *val);
struct pl *iccustom_aufile(const struct pl *val);
int iccustom_handler(const struct pl *pl, void *arg);

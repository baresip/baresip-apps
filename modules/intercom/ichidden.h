/**
 * @file ichidden.h Hidden DTMF call interface
 *
 * Copyright (C) 2022 Commend.com - c.spielberger@commend.com
 */

struct call;

int  call_hidden_start(struct call *call);
void call_hidden_close(struct call *call);
void call_hidden_dtmf_handler(struct call *call, char key, void *arg);

int  ichidden_init(void);
void ichidden_close(void);

/****************************************************************************
 * Copyright (c) 2012, the Konoha project authors. All rights reserved.
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 ***************************************************************************/

#include <minikonoha/minikonoha.h>
#include <minikonoha/sugar.h>

#include <pthread.h>
#include <event.h>
#include <evhttp.h>
#include <jansson.h>

#ifdef __cplusplus
extern "C"{
#endif

/* ------------------------------------------------------------------------ */

typedef struct {
	KonohaObjectHeader h;
	json_t *j;
} kEvent;

static void Event_init(KonohaContext *kctx, kObject *o, void *conf)
{
	kEvent *event = (kEvent *)o;
	event->j = NULL;
}

static void Event_free(KonohaContext *kctx, kObject *o)
{
	kEvent *event = (kEvent *)o;
	if(event->j) {
		json_decref(event->j);
		event->j = NULL;
	}
}

static void Event_reftrace(KonohaContext *kctx, kObject *o)
{
}

/* ------------------------------------------------------------------------ */

static void Event_new(unsigned char *str)
{
}

static void httpEventHandler(struct evhttp_request *req, void *args) {
	struct evbuffer *body = evhttp_request_get_input_buffer(req);
	size_t len = evbuffer_get_length(body);
	unsigned char *requestLine;
	struct evbuffer *buf = evbuffer_new();

	switch(req->type) {
		case EVHTTP_REQ_POST:
			requestLine = evbuffer_pullup(body, -1);
			Event_new(requestLine);
			evhttp_send_reply(req, HTTP_OK, "OK", buf);
			break;
		default:
			evhttp_send_error(req, HTTP_BADREQUEST, "Available POST only");
			break;
	}
	evbuffer_free(buf);
}

typedef struct {
	const char *host;
	int port;
} targs_t;

static void *httpEventListener(void *args) {
	targs_t *targs = (targs_t *)args;
	const char *host = targs->host;
	int port = targs->port;
	struct event_base *base = event_base_new();
	struct evhttp *httpd = evhttp_new(base);
	if(evhttp_bind_socket(httpd, host, port) < 0) {
		pthread_exit(NULL);
	}
	evhttp_set_gencb(httpd, httpEventHandler, NULL);
	event_base_dispatch(base);
	evhttp_free(httpd);
	event_base_free(base);
	return NULL;
}

static KMETHOD HttpEventGenerator_start(KonohaContext *kctx, KonohaStack *sfp) {
	const char *host = S_text(sfp[1].asString);
	int port = sfp[2].intValue;
	pthread_t t;
	targs_t targs = {
		host,
		port,
	};
	pthread_create(&t, NULL, httpEventListener, (void *)&targs);
}

/* ------------------------------------------------------------------------ */

#define _Public   kMethod_Public
#define _Static   kMethod_Static
#define _F(F)   (intptr_t)(F)

#define TY_HttpEventGenerator     cHttpEventGenerator->typeId

static kbool_t dse2_initPackage(KonohaContext *kctx, kNameSpace *ns, int argc, const char**args, kfileline_t pline)
{
	KDEFINE_CLASS defEvent = {
		STRUCTNAME(Event),
		.cflag = kClass_Final,
		.init = Event_init,
		.reftrace = Event_reftrace,
		.free = Event_free,
	};
	KDEFINE_CLASS defHttpEventGenerator = {
		.structname = "HttpEventGenerator",
		.typeId = TY_newid,
	};
	KonohaClass *cEvent = KLIB kNameSpace_defineClass(kctx, ns, NULL, &defEvent, pline);
	KonohaClass *cHttpEventGenerator = KLIB kNameSpace_defineClass(kctx, ns, NULL, &defHttpEventGenerator, pline);

	KDEFINE_METHOD MethodData[] = {
		_Public|_Static, _F(HttpEventGenerator_start), TY_void, TY_HttpEventGenerator, MN_("start"), 2, TY_String, FN_("host"), TY_int, FN_("port"),
		DEND,
	};
	KLIB kNameSpace_loadMethodData(kctx, ns, MethodData);

	return true;
}

static kbool_t dse2_setupPackage(KonohaContext *kctx, kNameSpace *ns, isFirstTime_t isFirstTime, kfileline_t pline)
{
	return true;
}

static kbool_t dse2_initNameSpace(KonohaContext *kctx, kNameSpace *packageNameSpace, kNameSpace *ns, kfileline_t pline)
{
	return true;
}

static kbool_t dse2_setupNameSpace(KonohaContext *kctx, kNameSpace *packageNameSpace, kNameSpace *ns, kfileline_t pline)
{
	return true;
}

KDEFINE_PACKAGE* dse2_init(void)
{
	static KDEFINE_PACKAGE d = {0};
	KSETPACKNAME(d, "dse2", "1.0");
	d.initPackage    = dse2_initPackage;
	d.setupPackage   = dse2_setupPackage;
	d.initNameSpace  = dse2_initNameSpace;
	d.setupNameSpace = dse2_setupNameSpace;
	return &d;
}

#ifdef __cplusplus
}
#endif

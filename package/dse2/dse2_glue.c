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

#define keventshare ((keventshare_t *)kctx->modshare[MOD_EVENT])
#define FLAG_EVENT (1 << 0)
#define CT_Event (keventshare->cEvent)

typedef struct {
	KonohaModule h;
	int flag;
	kFunc *invoke_func;
	kFunc *enq_func;
	KonohaClass *cEvent;
} keventshare_t;

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
	// TODO
}

/* ------------------------------------------------------------------------ */

static void Event_new(KonohaContext *kctx, unsigned char *str)
{
	json_t *root;
	json_error_t error;

	root = json_loads((const char *)str, 0, &error);
	kEvent *ev = (kEvent *)KLIB new_kObject(kctx, CT_Event, 0);
	ev->j = root;

	ktype_t resolve_type = Method_returnType(keventshare->enq_func->mtd);
	BEGIN_LOCAL(lsfp, K_CALLDELTA+1);
	KSETv_AND_WRITE_BARRIER(NULL, lsfp[K_CALLDELTA+0].o, K_NULL, GC_NO_WRITE_BARRIER);
	lsfp[K_CALLDELTA+1].o = (kObject *)ev;
	KCALL(lsfp, 0, keventshare->enq_func->mtd, 1, KLIB Knull(kctx, CT_(resolve_type)));
	END_LOCAL();
}

static void httpEventHandler(struct evhttp_request *req, void *args) {
	KonohaContext *kctx = (KonohaContext *)args;
	struct evbuffer *body = evhttp_request_get_input_buffer(req);
	size_t len = evbuffer_get_length(body);
	unsigned char *requestLine;
	requestLine[len] = '\0';
	struct evbuffer *buf = evbuffer_new();

	switch(req->type) {
		case EVHTTP_REQ_POST:
			requestLine = evbuffer_pullup(body, -1);
			Event_new(kctx, requestLine);
			evhttp_send_reply(req, HTTP_OK, "OK", buf);
			break;
		default:
			evhttp_send_error(req, HTTP_BADREQUEST, "Available POST only");
			break;
	}
	evbuffer_free(buf);
}

typedef struct {
	KonohaContext *kctx;
	const char *host;
	int port;
} targs_t;

static void *httpEventListener(void *args) {
	targs_t *targs = (targs_t *)args;
	KonohaContext *kctx = targs->kctx;
	const char *host = targs->host;
	int port = targs->port;
	struct event_base *base = event_base_new();
	struct evhttp *httpd = evhttp_new(base);
	if(evhttp_bind_socket(httpd, host, port) < 0) {
		pthread_exit(NULL);
	}
	evhttp_set_gencb(httpd, httpEventHandler, (void *)kctx);
	event_base_dispatch(base);
	evhttp_free(httpd);
	event_base_free(base);
	return NULL;
}

static KMETHOD HttpEventGenerator_start(KonohaContext *kctx, KonohaStack *sfp) {
	const char *host = S_text(sfp[1].asString);
	int port = sfp[2].intValue;
	pthread_t t;
	static targs_t targs = {};
	targs.kctx = kctx;
	targs.host = host;
	targs.port = port;
	pthread_create(&t, NULL, httpEventListener, (void *)&targs);
}

/* ------------------------------------------------------------------------ */
#define CHECK_JSON(obj, ret_stmt) do {\
		if (!json_is_object(obj)) {\
			DBG_P("[ERROR]: Object is not Json object.");\
			/*KLIB KonohaRuntime_raise(kctx, 1, sfp, pline, msg);*/\
			ret_stmt;\
		}\
	} while(0);

//## String Event.getProperty(String key);
static KMETHOD Event_getProperty(KonohaContext *kctx, KonohaStack *sfp)
{
	json_t* obj = ((kEvent*)sfp[0].asObject)->j;
	CHECK_JSON(obj, RETURN_(KNULL(String)));
	const char *key = S_text(sfp[1].asString);
	json_t* ret = json_object_get(obj, key);
	if (!json_is_string(ret)) {
		RETURN_(KNULL(String));
	}
	ret = json_incref(ret);
	const char* str = json_string_value(ret);
	if (str == NULL) {
		RETURN_(KNULL(String));
	}
	RETURN_(KLIB new_kString(kctx, str, strlen(str), 0));
}

/* ------------------------------------------------------------------------ */
//## void System.setSafepoint();
static KMETHOD System_setSafepoint(KonohaContext *kctx, KonohaStack *sfp)
{
	keventshare->flag |= FLAG_EVENT;
	((KonohaContextVar *)kctx)->safepoint = 1;
	RETURNvoid_();
}

//## void System.setEventInvokeFunc(Func f);
static KMETHOD System_setEventInvokeFunc(KonohaContext *kctx, KonohaStack *sfp)
{
	keventshare->invoke_func = sfp[1].asFunc;
	RETURNvoid_();
}

//## void System.setEnqFunc(Func f);
static KMETHOD System_setEnqFunc(KonohaContext *kctx, KonohaStack *sfp)
{
	keventshare->enq_func = sfp[1].asFunc;
	RETURNvoid_();
}

/* ------------------------------------------------------------------------ */

static void KscheduleEvent(KonohaContext *kctx) {
	// dispatch
	if(keventshare->flag & FLAG_EVENT) {
		keventshare->flag ^= FLAG_EVENT;
		BEGIN_LOCAL(lsfp, K_CALLDELTA);
		KCALL(lsfp, 0, keventshare->invoke_func->mtd, 0, K_NULL);
		END_LOCAL();
	}
}

static void keventshare_setup(KonohaContext *kctx, struct KonohaModule *def, int newctx)
{
}

static void keventshare_reftrace(KonohaContext *kctx, struct KonohaModule *baseh)
{
	keventshare_t *base = (keventshare_t *)baseh;
	BEGIN_REFTRACE(2);
	KREFTRACEv(base->invoke_func);
	KREFTRACEv(base->enq_func);
	END_REFTRACE();
}

static void keventshare_free(KonohaContext *kctx, struct KonohaModule *baseh)
{
	KFREE(baseh, sizeof(keventshare_t));
}

/* ------------------------------------------------------------------------ */

#define _Public   kMethod_Public
#define _Static   kMethod_Static
#define _Const    kMethod_Const
#define _Im       kMethod_Immutable
#define _F(F)   (intptr_t)(F)

#define TY_Event cEvent->typeId
#define TY_HttpEventGenerator cHttpEventGenerator->typeId

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

	keventshare_t *base = (keventshare_t*)KCALLOC(sizeof(keventshare_t), 1);
	base->h.name     = "event";
	base->h.setup    = keventshare_setup;
	base->h.reftrace = keventshare_reftrace;
	base->h.free     = keventshare_free;
	base->flag = 0;
	base->cEvent = cEvent;
	KINITv(base->invoke_func, K_NULL);
	KSET_KLIB(KscheduleEvent, 0);
	KLIB KonohaRuntime_setModule(kctx, MOD_EVENT, &base->h, pline);

	kparamtype_t P_Func[] = {{TY_Event}};
	int TY_EnqFunc = (KLIB KonohaClass_Generics(kctx, CT_Func, TY_void, 1, P_Func))->typeId;

	KDEFINE_METHOD MethodData[] = {
		/* event gen */
		_Public|_Static, _F(HttpEventGenerator_start), TY_void, TY_HttpEventGenerator, MN_("start"), 2, TY_String, FN_("host"), TY_int, FN_("port"),
		/* event */
		_Public|_Const|_Im, _F(Event_getProperty), TY_String,    TY_Event, MN_("getProperty"), 1, TY_String, FN_("key"),

		/* dispatch */
		_Public|_Static, _F(System_setSafepoint), TY_void, TY_System, MN_("setSafepoint"), 0,
		_Public|_Static, _F(System_setEventInvokeFunc), TY_void, TY_System, MN_("setEventInvokeFunc"), 1, TY_Func, FN_("f"),
		_Public|_Static, _F(System_setEnqFunc), TY_void, TY_System, MN_("setEnqFunc"), 1, TY_EnqFunc, FN_("f"),
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

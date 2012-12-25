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
#include <minikonoha/klib.h>
#include <minikonoha/konoha_common.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	const char *host;
	int port;
} Ip;

/* ------------------------------------------------------------------------ */
//## void System.startEventHandler(String host, int port);
static KMETHOD System_startEventHandler(KonohaContext *kctx, KonohaStack *sfp)
{
	const char *host = kString_text(sfp[1].asString);
	int port = sfp[2].intValue;
	Ip ip = {};
	ip.host = host;
	ip.port = port;
	PLATAPI StartEventHandler(kctx, (void *)&ip);
	KReturnVoid();
}

//## void System.stopEventHandler();
static KMETHOD System_stopEventHandler(KonohaContext *kctx, KonohaStack *sfp)
{
	PLATAPI StopEventHandler(kctx, NULL);
	KReturnVoid();
}

//## boolean System.emitEvent(Json json);
static KMETHOD System_emitEvent(KonohaContext *kctx, KonohaStack *sfp)
{
}

kbool_t consume(KonohaContext *kctx, struct JsonBuf *buf, KTraceInfo *trace, void *args)
{
//	kFunc *consumer = (kFunc *)args;
//	BEGIN_UnusedStack(lsfp);
//	KUnsafeFieldSet(lsfp[0].asObject, (kObject *)buf);
//	KStackSetFuncAll(lsfp, KLIB Knull(kctx, KLIB kNameSpace_GetClassByFullName(kctx, NULL, "Json", 4, NULL)), 0/*UL*/, consumer, 1);
//	KStackCall(lsfp);
//	END_UnusedStack();
	return true;
}

//## void System.dispatchEvent(Func consumer);
static KMETHOD System_dispatchEvent(KonohaContext *kctx, KonohaStack *sfp)
{
//	PLATAPI DispatchEvent(kctx, consume, NULL);
}

//## void System.waitEvent(Func consumer);
static KMETHOD System_waitEvent(KonohaContext *kctx, KonohaStack *sfp)
{
}

/* ------------------------------------------------------------------------ */

#define _Public   kMethod_Public
#define _Const    kMethod_Const
#define _Static   kMethod_Static
#define _Im       kMethod_Immutable
#define _F(F)   (intptr_t)(F)

static kbool_t event_PackupNameSpace(KonohaContext *kctx, kNameSpace *ns, int option, KTraceInfo *trace)
{
	PLATAPI EnterEventContext(kctx, NULL);
	KDEFINE_METHOD MethodData[] = {
		_Public|_Const|_Im|_Static, _F(System_startEventHandler), KType_void, KType_System , KKMethodName_("startEventHandler"), 2, KType_String, KFieldName_("host"), KType_int, KFieldName_("port"),
		_Public|_Const|_Im|_Static, _F(System_stopEventHandler), KType_void, KType_System , KKMethodName_("stopEventHandler"), 0,
		DEND,
	};
	KLIB kNameSpace_LoadMethodData(kctx, ns, MethodData, trace);
	return true;
}

static kbool_t event_ExportNameSpace(KonohaContext *kctx, kNameSpace *ns, kNameSpace *exportNS, int option, KTraceInfo *trace)
{
	return true;
}

KDEFINE_PACKAGE* event_Init(void)
{
	static KDEFINE_PACKAGE d = {
		KPACKNAME("event", "1.0"),
		.PackupNameSpace    = event_PackupNameSpace,
		.ExportNameSpace   = event_ExportNameSpace,
	};
	return &d;
}

#ifdef __cplusplus
}
#endif

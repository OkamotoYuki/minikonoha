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

#include <string.h>
#include <curl/curl.h>

#include <konoha3/konoha.h>
#include <konoha3/sugar.h>
#include <konoha3/import/methoddecl.h>

#define JSONAPI PLATAPI JsonModule.

struct JsonBuf {
	uint64_t json_i;
};

#ifdef __cplusplus
extern "C"{
#endif

static kbool_t ThrowExceptionInfoToEventListener(KonohaContext *kctx, struct JsonBuf *info)
{
	if(info == NULL || JSONAPI IsJsonType(info, KJSON_NULL)) {
		return false;
	}
	const char *text = JSONAPI JsonToNewText(kctx, info);
	CURL *curl = curl_easy_init();
	curl_easy_setopt(curl, CURLOPT_URL, "http://127.0.0.1:8080");
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, text);
	CURLcode res = curl_easy_perform(curl);
	curl_easy_cleanup(curl);
	switch(res) {
		case CURLE_OK:
			return true;
		default:
			return false;
	}
}

static const char* BeginTag(KonohaContext *kctx, kinfotag_t t)
{
	DBG_ASSERT(t <= NoneTag);
	if(!KonohaContext_Is(Interactive, kctx)) t = NoneTag;
	static const char* tags[] = {
		"\x1b[1m\x1b[31m", /*CritTag*/
		"\x1b[1m\x1b[31m", /*ErrTag*/
		"\x1b[1m\x1b[31m", /*WarnTag*/
		"\x1b[1m", /*NoticeTag*/
		"\x1b[1m", /*InfoTag*/
		"", /*DebugTag*/
		"", /* NoneTag*/
	};
	return tags[(int)t];
}

static const char* EndTag(KonohaContext *kctx, kinfotag_t t)
{
	DBG_ASSERT(t <= NoneTag);
	if(!KonohaContext_Is(Interactive, kctx)) t = NoneTag;
	static const char* tags[] = {
			"\x1b[0m", /*CritTag*/
			"\x1b[0m", /*ErrTag*/
			"\x1b[0m", /*WarnTag*/
			"\x1b[0m", /*NoticeTag*/
			"\x1b[0m", /*InfoTag*/
			"", /* Debug */
			"", /* NoneTag*/
	};
	return tags[(int)t];
}

static void KBuffer_WriteValue(KonohaContext *kctx, KBuffer *wb, KClass *c, KonohaStack *sfp)
{
	if(KClass_Is(UnboxType, c)) {
		c->p(kctx, sfp, 0, wb);
	}
	else {
		KLIB kObject_WriteToBuffer(kctx, sfp[0].asObject, false/*delim*/, wb, NULL, 0);
	}
}

static kbool_t AppendStringArrayToJson(KonohaContext *kctx, struct JsonBuf *jsonbuf, const char *text)
{
	struct JsonBuf value;
	JSONAPI CreateJson(kctx, &value, KJSON_STRING, text);
	if(!JSONAPI AppendJsonArray(kctx, jsonbuf, &value)) {
		return false;
	}
	return true;
}

#define CreateExceptionInfo() \
	struct JsonBuf info;\
	JSONAPI CreateJson(kctx, &info, KJSON_OBJECT);

#define AddExceptionInfoString(KEY, VAL) JSONAPI SetJsonValue(kctx, &info, KEY, strlen(KEY), KJSON_STRING, VAL)

#define AddExceptionInfoJson(KEY, VAL) JSONAPI SetJsonKeyValue(kctx, &info, KEY, strlen(KEY), VAL)

#define ThrowExceptionInfo() ThrowExceptionInfoToEventListener(kctx, &info)

#define DestroyExceptionInfo() JSONAPI FreeJson(kctx, &info)

static void UI_ReportCaughtException(KonohaContext *kctx, kException *e, KonohaStack *bottomStack, KonohaStack *topStack)
{
	DBG_ASSERT(IS_Exception(e));
	CreateExceptionInfo();
	const char *exceptionName = KSymbol_text(e->symbol);
	const char *optionalMessage = kString_text(e->Message);
	AddExceptionInfoString("event", exceptionName);
	AddExceptionInfoString("optionalMessage", optionalMessage);
	int fault = e->fault;
	struct JsonBuf faultInfo;
	JSONAPI CreateJson(kctx, &faultInfo, KJSON_OBJECT);
	PLATAPI printf_i("%s", BeginTag(kctx, ErrTag));
	if(optionalMessage != NULL && optionalMessage[0] != 0) {
		AppendStringArrayToJson(kctx, &faultInfo, "SoftwareFault");
		PLATAPI printf_i("%s: SoftwareFault %s", exceptionName, optionalMessage);
	}
	else {
		PLATAPI printf_i("%s:", exceptionName);
		if(KFlag_Is(int, fault, SoftwareFault)) {
			AppendStringArrayToJson(kctx, &faultInfo, "SoftwareFault");
			PLATAPI printf_i(" SoftwareFault");
		}
		if(KFlag_Is(int, fault, UserFault)) {
			AppendStringArrayToJson(kctx, &faultInfo, "UserFault");
			PLATAPI printf_i(" UserFault");
		}
		if(KFlag_Is(int, fault, SystemFault)) {
			AppendStringArrayToJson(kctx, &faultInfo, "SystemFault");
			PLATAPI printf_i(" SystemFault");
		}
		if(KFlag_Is(int, fault, ExternalFault)) {
			AppendStringArrayToJson(kctx, &faultInfo, "ExternalFault");
			PLATAPI printf_i(" ExternalFault");
		}
	}
	AddExceptionInfoJson("fault", &faultInfo);
	PLATAPI printf_i("%s\n", EndTag(kctx, ErrTag));
	PLATAPI printf_i("%sStackTrace\n", BeginTag(kctx, InfoTag));

	KonohaStack *sfp = topStack;
	KBuffer wb;
	KLIB KBuffer_Init(&(kctx->stack->cwb), &wb);
	struct JsonBuf StackTraceInfo;
	struct JsonBuf stackInfo;
	JSONAPI CreateJson(kctx, &StackTraceInfo, KJSON_OBJECT);
	while(bottomStack < sfp) {
		JSONAPI CreateJson(kctx, &stackInfo, KJSON_OBJECT);
		kMethod *mtd = sfp[K_MTDIDX].calledMethod;
		kfileline_t uline = sfp[K_RTNIDX].calledFileLine;
		const char *file = PLATAPI shortFilePath(KFileLine_textFileName(uline));
		JSONAPI SetJsonValue(kctx, &stackInfo, "file", strlen("file"), KJSON_STRING, file);
		// add more info
		PLATAPI printf_i(" [%ld] (%s:%d) %s.%s%s(", (sfp - kctx->stack->stack), file, (kushort_t)uline, kMethod_Fmt3(mtd));
		KClass *cThis = KClass_(mtd->typeId);
		if(!KClass_Is(UnboxType, cThis)) {
			cThis = kObject_class(sfp[0].asObject);
		}
		if(!kMethod_Is(Static, mtd)) {
			KBuffer_WriteValue(kctx, &wb, cThis, sfp);
			PLATAPI printf_i("this=(%s) %s, ", KClass_text(cThis), KLIB KBuffer_text(kctx, &wb, 1));
			KLIB KBuffer_Free(&wb);
		}
		unsigned i;
		kParam *param = kMethod_GetParam(mtd);
		for(i = 0; i < param->psize; i++) {
			if(i > 0) {
				PLATAPI printf_i(", ");
			}
			KClass *c = KClass_(param->paramtypeItems[i].attrTypeId);
			c = c->realtype(kctx, c, cThis);
			KBuffer_WriteValue(kctx, &wb, c, sfp + i + 1);
			PLATAPI printf_i("%s=(%s) %s", KSymbol_text(KSymbol_Unmask(param->paramtypeItems[i].name)), KClass_text(c), KLIB KBuffer_text(kctx, &wb, 1));
			KLIB KBuffer_Free(&wb);
		}
		PLATAPI printf_i(")\n");
		sfp = sfp[K_SHIFTIDX].previousStack;
		JSONAPI AppendJsonArray(kctx, &StackTraceInfo, &stackInfo);
	}
	AddExceptionInfoJson("StackTraceInfo", &StackTraceInfo);
	KLIB KBuffer_Free(&wb);
	PLATAPI printf_i("%s\n", EndTag(kctx, InfoTag));
	ThrowExceptionInfo();
	DestroyExceptionInfo();
	KLIB KscheduleEvent(kctx);
}

static kbool_t ExceptionEvent_PackupNameSpace(KonohaContext *kctx, kNameSpace *ns, int option, KTraceInfo *trace)
{
	KonohaFactory *factory = (KonohaFactory *)kctx->platApi;
	factory->ConsoleModule.ReportCaughtException = UI_ReportCaughtException;
	return true;
}

static kbool_t ExceptionEvent_ExportNameSpace(KonohaContext *kctx, kNameSpace *ns, kNameSpace *exportNS, int option, KTraceInfo *trace)
{
	return true;
}

KDEFINE_PACKAGE *ExceptionEvent_Init(void)
{
	static KDEFINE_PACKAGE d = {
		KPACKNAME("exception", "1.0"),
		.PackupNameSpace    = ExceptionEvent_PackupNameSpace,
		.ExportNameSpace   = ExceptionEvent_ExportNameSpace,
	};
	return &d;
}

#ifdef __cplusplus
}
#endif

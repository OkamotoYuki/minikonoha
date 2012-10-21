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

#define USE_FILE 1
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <errno.h>
#include <stdio.h>

#include <minikonoha/minikonoha.h>
#include <minikonoha/sugar.h>
#include <minikonoha/konoha_common.h>

#ifdef __cplusplus
extern "C"{
#endif

#ifndef K_PATHMAX
#define K_PATHMAX 1024
#endif

#define LogFileName(S)     LogText("filename", S)
#define LogFile(F) LogText("filename", kFile_textPath(kctx, F))

static const char* kFile_textPath(KonohaContext *kctx, kFile *file)
{
	return (file->PathInfoNULL == NULL) ? "unknown" : S_text(file->PathInfoNULL);
}

static int TRACE_fgetc(KonohaContext *kctx, kFile *file, KTraceInfo *trace)
{
	int ch = fgetc(file->fp);
	if(ferror(file->fp) != 0) {
		KTraceErrorPoint(trace, SystemFault, "fgetc", LogFile(file), LogErrno);
		KLIB KonohaRuntime_raise(kctx, EXPT_("IO"), SystemFault, NULL, trace->baseStack);
	}
	return ch;
}

static int TRACE_fputc(KonohaContext *kctx, kFile *file, int ch, KTraceInfo *trace)
{
	if(fputc(ch, file->fp) != 0) {
		KTraceErrorPoint(trace, SystemFault, "fputc", LogFile(file), LogErrno);
		KLIB KonohaRuntime_raise(kctx, EXPT_("IO"), SystemFault, NULL, trace->baseStack);
	}
	return ch;
}

static size_t TRACE_fread(KonohaContext *kctx, kFile *file, char *buf, size_t bufsiz, KTraceInfo *trace)
{
	size_t size = fread(buf, 1, bufsiz, file->fp);
	if(ferror(file->fp) != 0){
		KTraceErrorPoint(trace, SystemFault, "fread", LogFile(file), LogErrno);
		KLIB KonohaRuntime_raise(kctx, EXPT_("IO"), SystemFault, NULL, trace->baseStack);
	}
	return size;
}

static size_t TRACE_fwrite(KonohaContext *kctx, kFile *file, const char *buf, size_t bufsiz, KTraceInfo *trace)
{
	size_t size = fwrite(buf, 1, bufsiz, file->fp);
	if(ferror(file->fp) != 0){
		KTraceErrorPoint(trace, SystemFault, "fwrite", LogFile(file), LogErrno);
		KLIB KonohaRuntime_raise(kctx, EXPT_("IO"), SystemFault, NULL, trace->baseStack);
	}
	return size;
}

/* ------------------------------------------------------------------------ */

static void File_init(KonohaContext *kctx, kObject *o, void *conf)
{
	struct kFileVar *file = (struct kFileVar *)o;
	file->fp = (conf != NULL) ? conf : NULL;
	file->PathInfoNULL = NULL;
	file->readerIconv = ICONV_NULL;
	file->writerIconv = ICONV_NULL;
}

static void kFile_close(KonohaContext *kctx, kFile *file, KTraceInfo *trace)
{
	DBG_ASSERT(file->fp != NULL);
	int ret = fclose(file->fp);
	if(ret != 0) {
		KTraceErrorPoint(trace, SoftwareFault|SystemFault, "fclose", LogErrno);
	}
	file->fp = NULL;
	if(file->readerIconv != ICONV_NULL) {
		PLATAPI iconv_close_i(kctx, file->readerIconv);
		file->readerIconv = ICONV_NULL;
	}
	if(file->writerIconv != ICONV_NULL) {
		PLATAPI iconv_close_i(kctx, file->writerIconv);
		file->writerIconv = ICONV_NULL;
	}
}

static void kFile_checkEOF(KonohaContext *kctx, kFile *file, KTraceInfo *trace)
{
	DBG_ASSERT(file->fp != NULL);
	if(feof(file->fp) != 0) {
		kFile_close(kctx, file, trace);
	}
}

static void File_free(KonohaContext *kctx, kObject *o)
{
	struct kFileVar *file = (struct kFileVar *)o;
	if(file->fp != NULL) {
		kFile_close(kctx, file, NULL/*trace*/);
	}
}

static void File_p(KonohaContext *kctx, KonohaValue *v, int pos, KGrowingBuffer *wb)
{
	kFile *file = (kFile *)v[pos].asObject;
	if(file->PathInfoNULL != NULL) {
		KLIB Kwb_write(kctx, wb, S_text(file->PathInfoNULL), S_size(file->PathInfoNULL));
	}
	else {
		KLIB Kwb_printf(kctx, wb, "FILE:%p", file->fp);
	}
}

/* ------------------------------------------------------------------------ */

//## FILE System.fopen(String path, String mode);
static KMETHOD System_fopen(KonohaContext *kctx, KonohaStack *sfp)
{
	KMakeTrace(trace, sfp);
	char buffer[K_PATHMAX];
	kString *path = sfp[1].asString;
	const char *systemPath = PLATAPI formatSystemPath(kctx, buffer, K_PATHMAX, S_text(sfp[1].asString), S_size(sfp[1].asString), trace);
	const char *mode = S_text(sfp[2].asString);
	FILE *fp = fopen(systemPath, mode);
	if(fp == NULL) {
		int fault = PLATAPI diagnosisFaultType(kctx, kString_guessUserFault(path)|SystemError, trace);
		KTraceErrorPoint(trace, fault, "fopen",
			LogText("filename", S_text(path)), LogText("mode", mode), LogErrno);
		KLIB KonohaRuntime_raise(kctx, EXPT_("IO"), fault, NULL, sfp);
	}
	if(mode[0] == 'w' || mode[0] == 'a' || mode[1] == '+') {
		KTraceChangeSystemPoint(trace, "fopen", LogFileName(S_text(path)), LogText("mode", mode));
	}
	{
		INIT_GCSTACK();
		struct kFileVar *file = (struct kFileVar *)KLIB new_kObject(kctx, _GcStack, KGetReturnType(sfp), (uintptr_t)fp);
		KFieldInit(file, file->PathInfoNULL, path);
		if(!PLATAPI isSystemCharsetUTF8(kctx)) {
			if(mode[0] == 'w' || mode[0] == 'a' || mode[1] == '+') {
				file->writerIconv = PLATAPI iconvUTF8ToSystemCharset(kctx, trace);
			}
			else {
				file->readerIconv = PLATAPI iconvSystemCharsetToUTF8(kctx, trace);
			}
		}
		KReturnWith(file, RESET_GCSTACK());
	}
}

//## boolean System.fclose(FILE fp);
static KMETHOD System_fclose(KonohaContext *kctx, KonohaStack *sfp)
{
	kFile *file = sfp[1].asFile;
	if(file->fp != NULL) {
		KMakeTrace(trace, sfp);
		kFile_close(kctx, file, trace);
	}
	KReturnVoid();
}

//## @Native void File.close();
static KMETHOD File_close(KonohaContext *kctx, KonohaStack *sfp)
{
	kFile *file = (kFile *)sfp[0].asObject;
	if(file->fp != NULL) {
		KMakeTrace(trace, sfp);
		kFile_close(kctx, file, trace);
	}
	KReturnVoid();
}

//## @Native int File.getc();
static KMETHOD File_getc(KonohaContext *kctx, KonohaStack *sfp)
{
	kFile *file = (kFile *)sfp[0].asObject;
	KMakeTrace(trace, sfp);
	int ch = TRACE_fgetc(kctx, file, trace);
	KReturnUnboxValue(ch);
}

//## @Native void File.putc(int ch);
static KMETHOD File_putc(KonohaContext *kctx, KonohaStack *sfp)
{
	kFile *file = (kFile *)sfp[0].asObject;
	KMakeTrace(trace, sfp);
	TRACE_fputc(kctx, file, sfp[1].intValue, trace);
	KReturnVoid();
}

//## String File.readLine();
static KMETHOD File_readLine(KonohaContext *kctx, KonohaStack *sfp)
{
	kFile *file = (kFile *)sfp[0].asObject;
	KGrowingBuffer wb;
	KLIB Kwb_init(&(kctx->stack->cwb), &wb);
	int ch, pos = 0, hasUTF8 = false, bufferCount = 0, policy = StringPolicy_ASCII;
	char buffer[K_PAGESIZE];
	KMakeTrace(trace, sfp);
	while((ch == TRACE_fgetc(kctx, file, trace)) != EOF) {
		if(ch == '\r') continue;
		if(ch == '\n') {
			if(bufferCount == 0 && (!hasUTF8 || file->readerIconv == ICONV_NULL)) {
				KReturn(KLIB new_kString(kctx, OnStack, buffer, pos, policy));
			}
			break;
		}
		if(ch > 127) {
			hasUTF8 = true;
			policy = StringPolicy_UTF8;
		}
		buffer[pos] = ch; pos++;
		if(!(pos < K_PAGESIZE)) {
			if(hasUTF8 && file->readerIconv != ICONV_NULL) {
				KLIB Kwb_iconv(kctx, &wb, file->readerIconv, buffer, pos, trace);
			}
			else {
				KLIB Kwb_write(kctx, &wb, buffer, pos);
			}
			bufferCount++;
			hasUTF8 = false;
			pos = 0;
		}
	}
	if(pos > 0) {
		if(hasUTF8 && file->readerIconv != ICONV_NULL) {
			KLIB Kwb_iconv(kctx, &wb, file->readerIconv, buffer, pos, trace);
		}
		else {
			KLIB Kwb_write(kctx, &wb, buffer, pos);
		}
	}
	kFile_checkEOF(kctx, file, trace);
	KReturnWith(
		KLIB new_kString(kctx, OnStack, KLIB Kwb_top(kctx, &wb, 0), Kwb_bytesize(&wb), policy),
		KLIB Kwb_free(&wb)
	);
}

//## void File.print(String line);
static KMETHOD File_print(KonohaContext *kctx, KonohaStack *sfp)
{
	kFile   *file = sfp[0].asFile;
	kString *line = sfp[1].asString;
	KMakeTrace(trace, sfp);
	if(file->writerIconv == ICONV_NULL || kString_is(ASCII, line)) {
		TRACE_fwrite(kctx, file, S_text(line), S_size(line), trace);
	}
	else {
		KGrowingBuffer wb;
		KLIB Kwb_init(&(kctx->stack->cwb), &wb);
		KLIB Kwb_iconv(kctx, &wb, file->writerIconv, S_text(line), S_size(line), trace);
		TRACE_fwrite(kctx, file, KLIB Kwb_top(kctx, &wb, 0), Kwb_bytesize(&wb), trace);
		KLIB Kwb_free(&wb);
	}
}

//## void File.println();
static KMETHOD File_println0(KonohaContext *kctx, KonohaStack *sfp)
{
	kFile   *file = sfp[0].asFile;
	KMakeTrace(trace, sfp);
	TRACE_fwrite(kctx, file, TEXTSIZE("\n"), trace);
}

//## void File.println(String line);
static KMETHOD File_println(KonohaContext *kctx, KonohaStack *sfp)
{
	File_print(kctx, sfp);
	File_println0(kctx, sfp);
}



static void MethodLib_FileRead(KonohaContext *kctx, KonohaStack *sfp, kFile *file, kBytes *ba, size_t offset, size_t len)
{
	KCheckIndex(offset, ba->bytesize);
	KCheckIndex(offset+len, ba->bytesize);
	size_t size = fread(ba->buf + offset, 1, len, file->fp);
	KMakeTrace(trace, sfp);
	if(ferror(file->fp) != 0) {
		KTraceErrorPoint(trace, SystemFault, "fread", LogFile(file), LogErrno);
		KLIB KonohaRuntime_raise(kctx, EXPT_("IO"), SystemFault, NULL, sfp);
	}
	kFile_checkEOF(kctx, file, trace);
	KReturnUnboxValue(size);
}

//## int File.read(Bytes buf, int offset, int len);
static KMETHOD File_read3(KonohaContext *kctx, KonohaStack *sfp)
{
	MethodLib_FileRead(kctx, sfp, (kFile*)sfp[0].asObject, sfp[1].asBytes, sfp[2].intValue, sfp[3].intValue);
}

static void MethodLib_FileWrite(KonohaContext *kctx, KonohaStack *sfp, kFile *file, kBytes *ba, size_t offset, size_t len)
{
	KCheckIndex(offset, ba->bytesize);
	KCheckIndex(offset+len, ba->bytesize);
	size_t size = fwrite(ba->buf + offset, 1, len, file->fp);
	if(ferror(file->fp) != 0){
		KMakeTrace(trace, sfp);
		KTraceErrorPoint(trace, SystemFault, "fwrite", LogFile(file), LogErrno);
		KLIB KonohaRuntime_raise(kctx, EXPT_("IO"), SystemFault, NULL, sfp);
	}
	KReturnUnboxValue(size);
}

//## @Native int File.write(Bytes buf, int offset, int len);
static KMETHOD File_write3(KonohaContext *kctx , KonohaStack *sfp)
{
	MethodLib_FileWrite(kctx, sfp, (kFile*)sfp[0].asObject, sfp[1].asBytes, sfp[2].intValue, sfp[3].intValue);
}


// --------------------------------------------------------------------------

#define _Public   kMethod_Public
#define _Const    kMethod_Const
#define _Static   kMethod_Static
#define _Coercion kMethod_Coercion
#define _Im kMethod_Immutable
#define _F(F)   (intptr_t)(F)

static kbool_t file_initPackage(KonohaContext *kctx, kNameSpace *ns, int argc, const char**args, KTraceInfo *trace)
{
	KRequireKonohaCommonModule(trace);
	//KRequirePackage("konoha.bytes", trace);
	if(CT_File == NULL) {
		KDEFINE_CLASS defFile = {
			.structname = "FILE",
			.typeId = TY_newid,
			.cstruct_size = sizeof(kFile),
			.cflag = kClass_Final,
			.init  = File_init,
			.free  = File_free,
			.p     = File_p,
		};
		CT_File = KLIB kNameSpace_defineClass(kctx, ns, NULL, &defFile, trace);
	}
	KDEFINE_METHOD MethodData[] = {
		_Public|_Static, _F(System_fopen), TY_File, TY_System, MN_("fopen"), 2, TY_String, FN_("filename"), TY_String, FN_("mode"),
		_Public|_Static, _F(System_fclose), TY_void, TY_System, MN_("fclose"), 1, TY_File, FN_("file"),
		_Public, _F(File_close), TY_void, TY_File, MN_("close"), 0,
		_Public, _F(File_getc), TY_int, TY_File, MN_("getc"), 0,
		_Public, _F(File_putc), TY_void, TY_File, MN_("putc"), 1, TY_int, FN_("char"),
		_Public, _F(File_readLine), TY_String, TY_File, MN_("readLine"), 0,
		_Public, _F(File_print), TY_String, TY_File, MN_("print"), 1, TY_String, FN_("str"),
		_Public, _F(File_println), TY_String, TY_File, MN_("println"), 1, TY_String, FN_("str"),
		_Public, _F(File_println), TY_String, TY_File, MN_("println"), 0,
		DEND,
	};
	KLIB kNameSpace_loadMethodData(kctx, ns, MethodData);
	return true;
}

static kbool_t file_setupPackage(KonohaContext *kctx, kNameSpace *ns, isFirstTime_t isFirstTime, KTraceInfo *trace)
{
	if(CT_Bytes != NULL) {
		KDEFINE_METHOD MethodData[] = {
			_Public|_Im, _F(File_write3), TY_int, TY_File, MN_("write"), 3, TY_Bytes, FN_("buf"), TY_int, FN_("offset"), TY_int, FN_("len"),
			_Public|_Im, _F(File_read3),  TY_int, TY_File, MN_("read"), 3, TY_Bytes, FN_("buf"), TY_int, FN_("offset"), TY_int, FN_("len"),
			DEND,
		};
		KLIB kNameSpace_loadMethodData(kctx, ns, MethodData);
	}
	return true;
}

// --------------------------------------------------------------------------

KDEFINE_PACKAGE* file_init(void)
{
	static KDEFINE_PACKAGE d = {
		KPACKNAME("libc", "1.0"),
		.initPackage    = file_initPackage,
		.setupPackage   = file_setupPackage,
	};
	return &d;
}

#ifdef __cplusplus
}
#endif

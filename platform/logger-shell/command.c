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

#define K_USE_PTHREAD
#include <minikonoha/minikonoha.h>
#include <minikonoha/sugar.h>
#include "minikonoha/gc.h"
#include <minikonoha/klib.h>
#define USE_BUILTINTEST 1
#include "testkonoha.h"
#include <getopt.h>
#include <syslog.h>

#ifdef __cplusplus
extern "C" {
#endif

kstatus_t MODSUGAR_eval(KonohaContext *kctx, const char *script, size_t len, kfileline_t uline);
kstatus_t MODSUGAR_loadScript(KonohaContext *kctx, const char *path, size_t len, kfileline_t pline);

// -------------------------------------------------------------------------
// getopt

static int compileonly_flag = 0;
static int interactive_flag = 0;

extern int verbose_debug;
extern int verbose_code;
extern int verbose_sugar;
extern int verbose_gc;

#include <minikonoha/platform.h>

// -------------------------------------------------------------------------
// minishell

static char *(*kreadline)(const char *);
static int  (*kadd_history)(const char *);

static char* readline(const char* prompt)
{
	static int checkCTL = 0;
	int ch, pos = 0;
	static char linebuf[1024]; // THREAD-UNSAFE
	fputs(prompt, stdout);
	while((ch = fgetc(stdin)) != EOF) {
		if(ch == '\r') continue;
		if(ch == 27) {
			/* ^[[A */;
			fgetc(stdin); fgetc(stdin);
			if(checkCTL == 0) {
				fprintf(stdout, " - use readline, it provides better shell experience.\n");
				checkCTL = 1;
			}
			continue;
		}
		if(ch == '\n' || pos == sizeof(linebuf) - 1) {
			linebuf[pos] = 0;
			break;
		}
		linebuf[pos] = ch;
		pos++;
	}
	if(ch == EOF) return NULL;
	char *p = (char*)malloc(pos+1);
	memcpy(p, linebuf, pos+1);
	return p;
}

static int add_history(const char* line)
{
	return 0;
}

static int checkstmt(const char *t, size_t len)
{
	size_t i = 0;
	int ch, quote = 0, nest = 0;
	L_NORMAL:
	for(; i < len; i++) {
		ch = t[i];
		if(ch == '{' || ch == '[' || ch == '(') nest++;
		if(ch == '}' || ch == ']' || ch == ')') nest--;
		if(ch == '\'' || ch == '"' || ch == '`') {
			if(t[i+1] == ch && t[i+2] == ch) {
				quote = ch; i+=2;
				goto L_TQUOTE;
			}
		}
	}
	return nest;
	L_TQUOTE:
	DBG_ASSERT(i > 0);
	for(; i < len; i++) {
		ch = t[i];
		if(t[i-1] != '\\' && ch == quote) {
			if(t[i+1] == ch && t[i+2] == ch) {
				i+=2;
				goto L_NORMAL;
			}
		}
	}
	return 1;
}

static kstatus_t readstmt(KonohaContext *kctx, KUtilsWriteBuffer *wb, kfileline_t *uline)
{
	int line = 1;
	kstatus_t status = K_CONTINUE;
//	fputs(TERM_BBOLD(kctx), stdout);
	while(1) {
		int check;
		char *ln = kreadline(line == 1 ? ">>> " : "    ");
		if(ln == NULL) {
			KLIB Kwb_free(wb);
			status = K_BREAK;
			break;
		}
		if(line > 1) kwb_putc(wb, '\n');
		KLIB Kwb_write(kctx, wb, ln, strlen(ln));
		free(ln);
		if((check = checkstmt(KLIB Kwb_top(kctx, wb, 0), Kwb_bytesize(wb))) > 0) {
			uline[0]++;
			line++;
			continue;
		}
		if(check < 0) {
			fputs("(Cancelled)...\n", stdout);
			KLIB Kwb_free(wb);
		}
		break;
	}
	if(Kwb_bytesize(wb) > 0) {
		kadd_history(KLIB Kwb_top(kctx, wb, 1));
	}
//	fputs(TERM_EBOLD(kctx), stdout);
	fflush(stdout);
	uline[0]++;
	return status;
}

static void dumpEval(KonohaContext *kctx, KUtilsWriteBuffer *wb)
{
	KonohaStackRuntimeVar *base = kctx->stack;
	ktype_t ty = base->evalty;
	if(ty != TY_void) {
		KonohaStack *lsfp = base->stack + base->evalidx;
		CT_(ty)->p(kctx, lsfp, 0, wb, P_DUMP);
		fflush(stdout);
		fprintf(stdout, "TYPE=%s EVAL=%s\n", TY_t(ty), KLIB Kwb_top(kctx, wb,1));
	}
}

static void shell(KonohaContext *kctx)
{
	KUtilsWriteBuffer wb;
	KLIB Kwb_init(&(kctx->stack->cwb), &wb);
	kfileline_t uline = FILEID_("(shell)") | 1;
	while(1) {
		kfileline_t inc = 0;
		kstatus_t status = readstmt(kctx, &wb, &inc);
		if(status == K_BREAK) break;
		if(status == K_CONTINUE && Kwb_bytesize(&wb) > 0) {
			status = konoha_eval((KonohaContext*)kctx, KLIB Kwb_top(kctx, &wb, 1), uline);
			uline += inc;
			KLIB Kwb_free(&wb);
			if(status != K_FAILED) {
				dumpEval(kctx, &wb);
				KLIB Kwb_free(&wb);
			}
		}
	}
	KLIB Kwb_free(&wb);
	fprintf(stdout, "\n");
	return;
}

static void show_version(KonohaContext *kctx)
{
	int i;
	fprintf(stdout, K_PROGNAME " " K_VERSION " (%s) (%x, %s)\n", K_CODENAME, K_REVISION, __DATE__);
	fprintf(stdout, "[gcc %s]\n", __VERSION__);
	fprintf(stdout, "options:");
	for(i = 0; i < KonohaModule_MAXSIZE; i++) {
		if(kctx->modshare[i] != NULL) {
			fprintf(stdout, " %s", kctx->modshare[i]->name);
		}
	}
	fprintf(stdout, "\n");
}

static kbool_t konoha_shell(KonohaContext* konoha)
{
#ifdef __MINGW32__
	void *handler = (void *)LoadLibraryA((LPCTSTR)"libreadline" K_OSDLLEXT);
	void *f = (handler != NULL) ? (void *)GetProcAddress(handler, "readline") : NULL;
	kreadline = (f != NULL) ? (char* (*)(const char*))f : readline;
	f = (handler != NULL) ? (void *)GetProcAddress(handler, "add_history") : NULL;	
	kadd_history = (f != NULL) ? (int (*)(const char*))f : add_history;
#else
	void *handler = dlopen("libreadline" K_OSDLLEXT, RTLD_LAZY);
	void *f = (handler != NULL) ? dlsym(handler, "readline") : NULL;
	kreadline = (f != NULL) ? (char* (*)(const char*))f : readline;
	f = (handler != NULL) ? dlsym(handler, "add_history") : NULL;
	kadd_history = (f != NULL) ? (int (*)(const char*))f : add_history;
#endif
	show_version(konoha);
	shell(konoha);
	return true;
}


// -------------------------------------------------------------------------
// KonohaContext*est

static FILE *stdlog;
static int   stdlog_count = 0;

static const char* TEST_begin(kinfotag_t t)
{
	return "";
}

static const char* TEST_end(kinfotag_t t)
{
	return "";
}

static const char* TEST_shortText(const char *msg)
{
	return "(omitted..)";
}

static int TEST_vprintf(const char *fmt, va_list ap)
{
	stdlog_count++;
	return vfprintf(stdlog, fmt, ap);
}

static int TEST_printf(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int res = vfprintf(stdlog, fmt, ap);
	va_end(ap);
	return res;
}

static void TEST_reportCaughtException(const char *exceptionName, const char *scriptName, int line, const char *optionalMessage)
{
	if(line != 0) {
		fprintf(stdlog, " ** %s (%s:%d)\n", exceptionName, scriptName, line);
	}
	else {
		fprintf(stdlog, " ** %s\n", exceptionName);
	}
}

//static int check_result2(FILE *fp0, FILE *fp1)
//{
//	char buf0[128];
//	char buf1[128];
//	while (true) {
//		size_t len0, len1;
//		len0 = fread(buf0, 1, sizeof(buf0), fp0);
//		len1 = fread(buf1, 1, sizeof(buf1), fp1);
//		if (len0 != len1) {
//			return 1;//FAILED
//		}
//		if (len0 == 0) {
//			break;
//		}
//		if (memcmp(buf0, buf1, len0) != 0) {
//			return 1;//FAILED
//		}
//	}
//	return 0; //OK
//}

static int check_result2(FILE *fp0, FILE *fp1)
{
	char buf0[4096];
	char buf1[4096];
	while (fgets(buf0, sizeof(buf0), fp0) != NULL) {
		char *p = fgets(buf1, sizeof(buf1), fp1);
		if(p == NULL) return 1;//FAILED
		if((p = strstr(buf0, "(error) (")) != NULL) {
			p = strstr(p+8, ")");
			if(strncmp(buf0, buf1, p - buf1 + 1) != 0) return 1; //FAILED;
			continue;
		}
		if((p = strstr(buf0, "(warning) (")) != NULL) {
			p = strstr(p+10, ")");
			if(strncmp(buf0, buf1, p - buf1 + 1) != 0) return 1; //FAILED;
			continue;
		}
		if (strcmp(buf0, buf1) != 0) {
			return 1;//FAILED
		}
	}
	return 0; //OK
}

static void make_report(const char *testname)
{
	char *path = getenv("KONOHA_REPORT");
	if(path != NULL) {
		char report_file[256];
		char script_file[256];
		char correct_file[256];
		char result_file[256];
		snprintf(report_file, 256,  "%s/REPORT_%s.txt", path, shortFilePath(testname));
		snprintf(script_file, 256,  "%s", testname);
		snprintf(correct_file, 256, "%s.proof", script_file);
		snprintf(result_file, 256,  "%s.tested", script_file);
		FILE *fp = fopen(report_file, "w");
		FILE *fp2 = fopen(script_file, "r");
		int ch;
		while((ch = fgetc(fp2)) != EOF) {
			fputc(ch, fp);
		}
		fclose(fp2);
		fprintf(fp, "Expected Result (in %s)\n=====\n", result_file);
		fp2 = fopen(correct_file, "r");
		while((ch = fgetc(fp2)) != EOF) {
			fputc(ch, fp);
		}
		fclose(fp2);
		fprintf(fp, "Result (in %s)\n=====\n", result_file);
		fp2 = fopen(result_file, "r");
		while((ch = fgetc(fp2)) != EOF) {
			fputc(ch, fp);
		}
		fclose(fp2);
		fclose(fp);
	}
}

extern int konoha_detectFailedAssert;

static int KonohaContext_test(KonohaContext *kctx, const char *testname)
{
	int ret = 1; //FAILED
	char script_file[256];
	char correct_file[256];
	char result_file[256];
	PLATAPI snprintf_i(script_file, 256,  "%s", testname);
	PLATAPI snprintf_i(correct_file, 256, "%s.proof", script_file);
	PLATAPI snprintf_i(result_file, 256,  "%s.tested", script_file);
	FILE *fp = fopen(correct_file, "r");
	stdlog = fopen(result_file, "w");
	konoha_load((KonohaContext*)kctx, script_file);
	fprintf(stdlog, "Q.E.D.\n");   // Q.E.D.
	fclose(stdlog);

	if(fp != NULL) {
		FILE *fp2 = fopen(result_file, "r");
		ret = check_result2(fp, fp2);
		if(ret == 0) {
			fprintf(stdout, "[PASS]: %s\n", testname);
		}
		else {
			fprintf(stdout, "[FAIL]: %s\n", testname);
			make_report(testname);
			konoha_detectFailedAssert = 1;
		}
		fclose(fp);
		fclose(fp2);
	}
	else {
		//fprintf(stdout, "stdlog_count: %d\n", stdlog_count);
		if(stdlog_count == 0) {
			if(konoha_detectFailedAssert == 0) {
				fprintf(stdout, "[PASS]: %s\n", testname);
				return 0; // OK
			}
		}
		else {
			fprintf(stdout, "no proof file: %s\n", testname);
			konoha_detectFailedAssert = 1;
		}
		fprintf(stdout, "[FAIL]: %s\n", testname);
		return 1;
	}
	return ret;
}

#ifdef USE_BUILTINTEST
extern DEFINE_TESTFUNC KonohaTestSet[];
static BuiltInTestFunc lookupTestFunc(DEFINE_TESTFUNC *d, const char *name)
{
	while(d->name != NULL) {
		if(strcasecmp(name, d->name) == 0) {
			return d->f;
		}
		d++;
	}
	return NULL;
}
#endif

static int CommandLine_doBuiltInTest(KonohaContext* konoha, const char* name)
{
#ifdef USE_BUILTINTEST
	BuiltInTestFunc f = lookupTestFunc(KonohaTestSet, name);
	if(f != NULL) {
		return f(konoha);
	}
	fprintf(stderr, "Built-in test is not found: '%s'\n", name);
#else
	fprintf(stderr, "Built-in tests are not built; rebuild with -DUSE_BUILTINTEST\n");
#endif
	return 1;
}

static void CommandLine_define(KonohaContext *kctx, char *keyvalue)
{
	char *p = strchr(keyvalue, '=');
	if(p != NULL) {
		size_t len = p-keyvalue;
		char namebuf[len+1];
		memcpy(namebuf, keyvalue, len); namebuf[len] = 0;
		DBG_P("name='%s'", namebuf);
		ksymbol_t key = KLIB Ksymbol(kctx, namebuf, len, 0, SYM_NEWID);
		uintptr_t unboxValue;
		ktype_t ty;
		if(isdigit(p[1])) {
			ty = TY_int;
			unboxValue = (uintptr_t)strtol(p+1, NULL, 0);
		}
		else {
			ty = TY_TEXT;
			unboxValue = (uintptr_t)(p+1);
		}
		if(!KLIB kNameSpace_setConstData(kctx, KNULL(NameSpace), key, ty, unboxValue, 0)) {
			PLATAPI exit_i(EXIT_FAILURE);
		}
	}
	else {
		fprintf(stdout, "invalid define option: use -D<key>=<value>\n");
		PLATAPI exit_i(EXIT_FAILURE);
	}
}

static void CommandLine_import(KonohaContext *kctx, char *packageName)
{
	size_t len = strlen(packageName)+1;
	char bufname[len];
	memcpy(bufname, packageName, len);
	if(!(KLIB kNameSpace_importPackage(kctx, KNULL(NameSpace), bufname, 0))) {
		PLATAPI exit_i(EXIT_FAILURE);
	}
}

static void konoha_startup(KonohaContext *kctx, const char *startup_script)
{
	char buf[256];
	const char *path = PLATAPI getenv_i("KONOHA_SCRIPTPATH"), *local = "";
	if(path == NULL) {
		path = PLATAPI getenv_i("KONOHA_HOME");
		local = "/script";
	}
	if(path == NULL) {
		path = PLATAPI getenv_i("HOME");
		local = "/.minikonoha/script";
	}
	snprintf(buf, sizeof(buf), "%s%s/%s.k", path, local, startup_script);
	if(!konoha_load((KonohaContext*)kctx, (const char*)buf)) {
		PLATAPI exit_i(EXIT_FAILURE);
	}
}

static void CommandLine_setARGV(KonohaContext *kctx, int argc, char** argv)
{
	KonohaClass *CT_StringArray0 = CT_p0(kctx, CT_Array, TY_String);
	kArray *a = (kArray*)KLIB new_kObject(kctx, CT_StringArray0, 0);
	int i;
	for(i = 0; i < argc; i++) {
		DBG_P("argv=%d, '%s'", i, argv[i]);
		KLIB kArray_add(kctx, a, KLIB new_kString(kctx, argv[i], strlen(argv[i]), SPOL_TEXT));
	}
	KDEFINE_OBJECT_CONST ObjectData[] = {
			{"SCRIPT_ARGV", CT_StringArray0->typeId, (kObject*)a},
			{}
	};
	KLIB kNameSpace_loadConstData(kctx, KNULL(NameSpace), KonohaConst_(ObjectData), 0);
}

// -------------------------------------------------------------------------
// ** logger **

static char *write_byte_toebuf(const char *text, size_t len, char *p, char *ebuf)
{
	if(ebuf - p > len) {
		memcpy(p, text, len);
		return p+len;
	}
	return p;
}

static char *write_text_toebuf(const char *s, char *p, char *ebuf)
{
	if(p < ebuf) { p[0] = '"'; p++; }
	while(*s != 0 && p < ebuf) {
		if(*s == '"') {
			p[0] = '\"'; p++;
			if(p < ebuf) {p[0] = s[0]; p++;}
		}
		else if(*s == '\n') {
			p[0] = '\\'; p++;
			if(p < ebuf) {p[0] = 'n'; p++;}
		}
		else {
			p[0] = s[0]; p++;
		}
		s++;
	}
	if(p < ebuf) { p[0] = '"'; p++; }
	return p;
}

static void reverse(char *const start, char *const end, const int len)
{
	int i, l = len / 2;
	register char *s = start;
	register char *e = end - 1;
	for (i = 0; i < l; i++) {
		char tmp = *s;
		*s++ = *e;
		*e-- = tmp;
	}
}

static char *write_uint_toebuf(uintptr_t unboxValue, char *const p, const char *const end)
{
	int i = 0;
	while (p + i < end) {
		int tmp = unboxValue % 10;
		unboxValue /= 10;
		p[i] = '0' + tmp;
		++i;
		if (unboxValue == 0)
			break;
	}
	reverse(p, p + i, i);
	return p + i;
}

#define EBUFSIZ 1024

#define LOG_END 0
#define LOG_s   1
#define LOG_u   2

static uintptr_t logger_p(void *arg, va_list ap)
{
	char buf[EBUFSIZ], *p = buf, *ebuf =  p + (EBUFSIZ - 4);
	p[0] = '{'; p++;
	{
		int c = 0, logtype;
		while((logtype = va_arg(ap, int)) != LOG_END) {
			const char *key = va_arg(ap, const char*);
			if(c > 0 && p + 3 < ebuf) { p[0] = ','; p[1] = ' '; p+=2; }
			if(p < ebuf) { p[0] = '"'; p++; }
			p = write_byte_toebuf(key, strlen(key), p, ebuf);
			if(p + 3 < ebuf) { p[0] = '"'; p[1] = ':'; p[2] = ' '; p+=3; }
			switch(logtype) {
			case LOG_s: {
				const char *text = va_arg(ap, const char*);
				p = write_text_toebuf(text, p, ebuf);
				break;
			}
			case LOG_u: {
				p = write_uint_toebuf(va_arg(ap, uintptr_t), p, ebuf);
				break;
			}
			default:
				if(p + 4 < ebuf) { p[0] = 'n'; p[1] = 'u'; p[2] = 'l'; p[3] = 'l'; p+=4; }
			}
			c++;
		}
	}
	p[0] = '}'; p++;
	p[0] = '\n'; p++;
	p[0] = '\0';
	syslog(LOG_NOTICE, "%s", buf);
	return 0;// FIXME reference to log
}

static uintptr_t logger(void *arg, ...)
{
	va_list ap;
	va_start(ap, arg);
	uintptr_t ref = logger_p(arg, ap);
	va_end(ap);
	return ref;
}

#define trace(arg, ...) do {\
	logger(p, __VA_ARGS__, LOG_END);\
} while(0)

// -------------------------------------------------------------------------
// ** resource monitor **

#include <sys/dir.h>
#include <locale.h>
#include <fcntl.h>
#include <unistd.h>
#include "procps.h"

#ifndef HZ
#include <netinet/in.h>  /* htons */
#endif

static unsigned long long Hertz;   /* clock tick frequency */
static long smp_num_cpus;     /* number of CPUs */
static int have_privs;     /* boolean, true if setuid or similar */

/* obsolete */
unsigned long kb_main_shared;
/* old but still kicking -- the important stuff */
static unsigned long kb_main_buffers;
static unsigned long kb_main_cached;
static unsigned long kb_main_free;
static unsigned long kb_main_total;
static unsigned long kb_swap_free;
static unsigned long kb_swap_total;
/* recently introduced */
static unsigned long kb_high_free;
static unsigned long kb_high_total;
static unsigned long kb_low_free;
static unsigned long kb_low_total;
/* 2.4.xx era */
static unsigned long kb_active;
static unsigned long kb_inact_laundry;
static unsigned long kb_inact_dirty;
static unsigned long kb_inact_clean;
static unsigned long kb_inact_target;
static unsigned long kb_swap_cached;  /* late 2.4 and 2.6+ only */
/* derived values */
static unsigned long kb_swap_used;
static unsigned long kb_main_used;
/* 2.5.41+ */
static unsigned long kb_writeback;
static unsigned long kb_slab;
static unsigned long nr_reversemaps;
static unsigned long kb_committed_as;
static unsigned long kb_dirty;
static unsigned long kb_inactive;
static unsigned long kb_mapped;
static unsigned long kb_pagetables;
// seen on a 2.6.x kernel:
static unsigned long kb_vmalloc_chunk;
static unsigned long kb_vmalloc_total;
static unsigned long kb_vmalloc_used;
// seen on 2.6.24-rc6-git12
static unsigned long kb_anon_pages;
static unsigned long kb_bounce;
static unsigned long kb_commit_limit;
static unsigned long kb_nfs_unstable;
static unsigned long kb_swap_reclaimable;
static unsigned long kb_swap_unreclaimable;

// see include/linux/page-flags.h and mm/page_alloc.c
unsigned long vm_nr_dirty;           // dirty writable pages
unsigned long vm_nr_writeback;       // pages under writeback
unsigned long vm_nr_pagecache;       // pages in pagecache -- gone in 2.5.66+ kernels
unsigned long vm_nr_page_table_pages;// pages used for pagetables
unsigned long vm_nr_reverse_maps;    // includes PageDirect
unsigned long vm_nr_mapped;          // mapped into pagetables
unsigned long vm_nr_slab;            // in slab
unsigned long vm_pgpgin;             // kB disk reads  (same as 1st num on /proc/stat page line)
unsigned long vm_pgpgout;            // kB disk writes (same as 2nd num on /proc/stat page line)
unsigned long vm_pswpin;             // swap reads     (same as 1st num on /proc/stat swap line)
unsigned long vm_pswpout;            // swap writes    (same as 2nd num on /proc/stat swap line)
unsigned long vm_pgalloc;            // page allocations
unsigned long vm_pgfree;             // page freeings
unsigned long vm_pgactivate;         // pages moved inactive -> active
unsigned long vm_pgdeactivate;       // pages moved active -> inactive
unsigned long vm_pgfault;           // total faults (major+minor)
unsigned long vm_pgmajfault;       // major faults
unsigned long vm_pgscan;          // pages scanned by page reclaim
unsigned long vm_pgrefill;       // inspected by refill_inactive_zone
unsigned long vm_pgsteal;       // total pages reclaimed
unsigned long vm_kswapd_steal; // pages reclaimed by kswapd
// next 3 as defined by the 2.5.52 kernel
unsigned long vm_pageoutrun;  // times kswapd ran page reclaim
unsigned long vm_allocstall; // times a page allocator ran direct reclaim
unsigned long vm_pgrotated; // pages rotated to the tail of the LRU for immediate reclaim
// seen on a 2.6.8-rc1 kernel, apparently replacing old fields
static unsigned long vm_pgalloc_dma;          // 
static unsigned long vm_pgalloc_high;         // 
static unsigned long vm_pgalloc_normal;       // 
static unsigned long vm_pgrefill_dma;         // 
static unsigned long vm_pgrefill_high;        // 
static unsigned long vm_pgrefill_normal;      // 
static unsigned long vm_pgscan_direct_dma;    // 
static unsigned long vm_pgscan_direct_high;   // 
static unsigned long vm_pgscan_direct_normal; // 
static unsigned long vm_pgscan_kswapd_dma;    // 
static unsigned long vm_pgscan_kswapd_high;   // 
static unsigned long vm_pgscan_kswapd_normal; // 
static unsigned long vm_pgsteal_dma;          // 
static unsigned long vm_pgsteal_high;         // 
static unsigned long vm_pgsteal_normal;       // 
// seen on a 2.6.8-rc1 kernel
static unsigned long vm_kswapd_inodesteal;    //
static unsigned long vm_nr_unstable;          //
static unsigned long vm_pginodesteal;         //
static unsigned long vm_slabs_scanned;        //

typedef struct disk_stat{
	unsigned long long reads_sectors;
	unsigned long long written_sectors;
	char               disk_name [16];
	unsigned           inprogress_IO;
	unsigned           merged_reads;
	unsigned           merged_writes;
	unsigned           milli_reading;
	unsigned           milli_spent_IO;
	unsigned           milli_writing;
	unsigned           partitions;
	unsigned           reads;
	unsigned           weighted_milli_spent_IO;
	unsigned           writes;
}disk_stat;

typedef struct partition_stat{
	char partition_name [16];
	unsigned long long reads_sectors;
	unsigned           parent_disk;  // index into a struct disk_stat array
	unsigned           reads;
	unsigned           writes;
	unsigned           requested_writes;
}partition_stat;

typedef struct slab_cache{
	char name[48];
	unsigned active_objs;
	unsigned num_objs;
	unsigned objsize;
	unsigned objperslab;
}slab_cache;

#define P_G_SZ 20
typedef struct proc_t {
// 1st 16 bytes
    int
        tid,		// (special)       task id, the POSIX thread ID (see also: tgid)
    	ppid;		// stat,status     pid of parent process
    unsigned
        pcpu;           // stat (special)  %CPU usage (is not filled in by readproc!!!)
    char
    	state,		// stat,status     single-char code for process state (S=sleeping)
    	pad_1,		// n/a             padding
    	pad_2,		// n/a             padding
    	pad_3;		// n/a             padding
// 2nd 16 bytes
    unsigned long long
	utime,		// stat            user-mode CPU time accumulated by process
	stime,		// stat            kernel-mode CPU time accumulated by process
// and so on...
	cutime,		// stat            cumulative utime of process and reaped children
	cstime,		// stat            cumulative stime of process and reaped children
	start_time;	// stat            start time of process -- seconds since 1-1-70
#ifdef SIGNAL_STRING
    char
	// Linux 2.1.7x and up have 64 signals. Allow 64, plus '\0' and padding.
	signal[18],	// status          mask of pending signals, per-task for readtask() but per-proc for readproc()
	blocked[18],	// status          mask of blocked signals
	sigignore[18],	// status          mask of ignored signals
	sigcatch[18],	// status          mask of caught  signals
	_sigpnd[18];	// status          mask of PER TASK pending signals
#else
    long long
	// Linux 2.1.7x and up have 64 signals.
	signal,		// status          mask of pending signals, per-task for readtask() but per-proc for readproc()
	blocked,	// status          mask of blocked signals
	sigignore,	// status          mask of ignored signals
	sigcatch,	// status          mask of caught  signals
	_sigpnd;	// status          mask of PER TASK pending signals
#endif
    unsigned KLONG
	start_code,	// stat            address of beginning of code segment
	end_code,	// stat            address of end of code segment
	start_stack,	// stat            address of the bottom of stack for the process
	kstk_esp,	// stat            kernel stack pointer
	kstk_eip,	// stat            kernel instruction pointer
	wchan;		// stat (special)  address of kernel wait channel proc is sleeping in
    long
	priority,	// stat            kernel scheduling priority
	nice,		// stat            standard unix nice level of process
	rss,		// stat            resident set size from /proc/#/stat (pages)
	alarm,		// stat            ?
    // the next 7 members come from /proc/#/statm
	size,		// statm           total # of pages of memory
	resident,	// statm           number of resident set (non-swapped) pages (4k)
	share,		// statm           number of pages of shared (mmap'd) memory
	trs,		// statm           text resident set size
	lrs,		// statm           shared-lib resident set size
	drs,		// statm           data resident set size
	dt;		// statm           dirty pages
    unsigned long
	vm_size,        // status          same as vsize in kb
	vm_lock,        // status          locked pages in kb
	vm_rss,         // status          same as rss in kb
	vm_data,        // status          data size
	vm_stack,       // status          stack size
	vm_exe,         // status          executable size
	vm_lib,         // status          library size (all pages, not just used ones)
	rtprio,		// stat            real-time priority
	sched,		// stat            scheduling class
	vsize,		// stat            number of pages of virtual memory ...
	rss_rlim,	// stat            resident set size limit?
	flags,		// stat            kernel flags for the process
	min_flt,	// stat            number of minor page faults since process start
	maj_flt,	// stat            number of major page faults since process start
	cmin_flt,	// stat            cumulative min_flt of process and child processes
	cmaj_flt;	// stat            cumulative maj_flt of process and child processes
    char
	**environ,	// (special)       environment string vector (/proc/#/environ)
	**cmdline;	// (special)       command line string vector (/proc/#/cmdline)
    char
	// Be compatible: Digital allows 16 and NT allows 14 ???
    	euser[P_G_SZ],	// stat(),status   effective user name
    	ruser[P_G_SZ],	// status          real user name
    	suser[P_G_SZ],	// status          saved user name
    	fuser[P_G_SZ],	// status          filesystem user name
    	rgroup[P_G_SZ],	// status          real group name
    	egroup[P_G_SZ],	// status          effective group name
    	sgroup[P_G_SZ],	// status          saved group name
    	fgroup[P_G_SZ],	// status          filesystem group name
    	cmd[16];	// stat,status     basename of executable file in call to exec(2)
    struct proc_t
	*ring,		// n/a             thread group ring
	*next;		// n/a             various library uses
    int
	pgrp,		// stat            process group id
	session,	// stat            session id
	nlwp,		// stat,status     number of threads, or 0 if no clue
	tgid,		// (special)       task group ID, the POSIX PID (see also: tid)
	tty,		// stat            full device number of controlling terminal
        euid, egid,     // stat(),status   effective
        ruid, rgid,     // status          real
        suid, sgid,     // status          saved
        fuid, fgid,     // status          fs (used for file access only)
	tpgid,		// stat            terminal process group id
	exit_signal,	// stat            might not be SIGCHLD
	processor;      // stat            current (or most recent?) CPU
} proc_t;

#define BAD_OPEN_MESSAGE					\
"Error: /proc must be mounted\n"				\
"  To mount /proc at boot you need an /etc/fstab line like:\n"	\
"      /proc   /proc   proc    defaults\n"			\
"  In the meantime, run \"mount /proc /proc -t proc\"\n"

#define STAT_FILE    "/proc/stat"
static int stat_fd = -1;
#define UPTIME_FILE  "/proc/uptime"
static int uptime_fd = -1;
#define LOADAVG_FILE "/proc/loadavg"
//static int loadavg_fd = -1;
#define MEMINFO_FILE "/proc/meminfo"
static int meminfo_fd = -1;
#define VMINFO_FILE "/proc/vmstat"
static int vminfo_fd = -1;

// As of 2.6.24 /proc/meminfo seems to need 888 on 64-bit,
// and would need 1258 if the obsolete fields were there.
static char buf[2048];

/* This macro opens filename only if necessary and seeks to 0 so
 * that successive calls to the functions are more efficient.
 * It also reads the current contents of the file into the global buf.
 */
#define FILE_TO_BUF(filename, fd) do{				\
    static int local_n;						\
    if (fd == -1 && (fd = open(filename, O_RDONLY)) == -1) {	\
	fputs(BAD_OPEN_MESSAGE, stderr);			\
	fflush(NULL);						\
	_exit(102);						\
    }								\
    lseek(fd, 0L, SEEK_SET);					\
    if ((local_n = read(fd, buf, sizeof buf - 1)) < 0) {	\
	perror(filename);					\
	fflush(NULL);						\
	_exit(103);						\
    }								\
    buf[local_n] = '\0';					\
}while(0)

/* evals 'x' twice */
#define SET_IF_DESIRED(x,y) do{  if(x) *(x) = (y); }while(0)

/***********************************************************************
 * Some values in /proc are expressed in units of 1/HZ seconds, where HZ
 * is the kernel clock tick rate. One of these units is called a jiffy.
 * The HZ value used in the kernel may vary according to hacker desire.
 * According to Linus Torvalds, this is not true. He considers the values
 * in /proc as being in architecture-dependant units that have no relation
 * to the kernel clock tick rate. Examination of the kernel source code
 * reveals that opinion as wishful thinking.
 *
 * In any case, we need the HZ constant as used in /proc. (the real HZ value
 * may differ, but we don't care) There are several ways we could get HZ:
 *
 * 1. Include the kernel header file. If it changes, recompile this library.
 * 2. Use the sysconf() function. When HZ changes, recompile the C library!
 * 3. Ask the kernel. This is obviously correct...
 *
 * Linus Torvalds won't let us ask the kernel, because he thinks we should
 * not know the HZ value. Oh well, we don't have to listen to him.
 * Someone smuggled out the HZ value. :-)
 *
 * This code should work fine, even if Linus fixes the kernel to match his
 * stated behavior. The code only fails in case of a partial conversion.
 *
 * Recent update: on some architectures, the 2.4 kernel provides an
 * ELF note to indicate HZ. This may be for ARM or user-mode Linux
 * support. This ought to be investigated. Note that sysconf() is still
 * unreliable, because it doesn't return an error code when it is
 * used with a kernel that doesn't support the ELF note. On some other
 * architectures there may be a system call or sysctl() that will work.
 */

static void old_Hertz_hack(void){
  unsigned long long user_j, nice_j, sys_j, other_j;  /* jiffies (clock ticks) */
  double up_1, up_2, seconds;
  unsigned long long jiffies;
  unsigned h;
  char *restrict savelocale;

  savelocale = setlocale(LC_NUMERIC, NULL);
  setlocale(LC_NUMERIC, "C");
  do{
    FILE_TO_BUF(UPTIME_FILE,uptime_fd);  sscanf(buf, "%lf", &up_1);
    /* uptime(&up_1, NULL); */
    FILE_TO_BUF(STAT_FILE,stat_fd);
    sscanf(buf, "cpu %Lu %Lu %Lu %Lu", &user_j, &nice_j, &sys_j, &other_j);
    FILE_TO_BUF(UPTIME_FILE,uptime_fd);  sscanf(buf, "%lf", &up_2);
    /* uptime(&up_2, NULL); */
  } while((long long)( (up_2-up_1)*1000.0/up_1 )); /* want under 0.1% error */
  setlocale(LC_NUMERIC, savelocale);
  jiffies = user_j + nice_j + sys_j + other_j;
  seconds = (up_1 + up_2) / 2;
  h = (unsigned)( (double)jiffies/seconds/smp_num_cpus );
  /* actual values used by 2.4 kernels: 32 64 100 128 1000 1024 1200 */
  switch(h){
  case    9 ...   11 :  Hertz =   10; break; /* S/390 (sometimes) */
  case   18 ...   22 :  Hertz =   20; break; /* user-mode Linux */
  case   30 ...   34 :  Hertz =   32; break; /* ia64 emulator */
  case   48 ...   52 :  Hertz =   50; break;
  case   58 ...   61 :  Hertz =   60; break;
  case   62 ...   65 :  Hertz =   64; break; /* StrongARM /Shark */
  case   95 ...  105 :  Hertz =  100; break; /* normal Linux */
  case  124 ...  132 :  Hertz =  128; break; /* MIPS, ARM */
  case  195 ...  204 :  Hertz =  200; break; /* normal << 1 */
  case  247 ...  252 :  Hertz =  250; break;
  case  253 ...  260 :  Hertz =  256; break;
  case  393 ...  408 :  Hertz =  400; break; /* normal << 2 */
  case  790 ...  808 :  Hertz =  800; break; /* normal << 3 */
  case  990 ... 1010 :  Hertz = 1000; break; /* ARM */
  case 1015 ... 1035 :  Hertz = 1024; break; /* Alpha, ia64 */
  case 1180 ... 1220 :  Hertz = 1200; break; /* Alpha */
  default:
#ifdef HZ
    Hertz = (unsigned long long)HZ;    /* <asm/param.h> */
#else
    /* If 32-bit or big-endian (not Alpha or ia64), assume HZ is 100. */
    Hertz = (sizeof(long)==sizeof(int) || htons(999)==999) ? 100UL : 1024UL;
#endif
    fprintf(stderr, "Unknown HZ value! (%d) Assume %Ld.\n", h, Hertz);
  }
}

// same as:   euid != uid || egid != gid
#ifndef AT_SECURE
#define AT_SECURE      23     // secure mode boolean (true if setuid, etc.)
#endif

#ifndef AT_CLKTCK
#define AT_CLKTCK       17    // frequency of times()
#endif

#define NOTE_NOT_FOUND 42

extern char** environ;

/* for ELF executables, notes are pushed before environment and args */
static unsigned long find_elf_note(unsigned long findme){
  unsigned long *ep = (unsigned long *)environ;
  while(*ep++);
  while(*ep){
    if(ep[0]==findme) return ep[1];
    ep+=2;
  }
  return NOTE_NOT_FOUND;
}


static int check_for_privs(void){
  unsigned long rc = find_elf_note(AT_SECURE);
  if(rc==NOTE_NOT_FOUND){
    // not valid to run this code after UID or GID change!
    // (if needed, may use AT_UID and friends instead)
    rc = geteuid() != getuid() || getegid() != getgid();
  }
  return !!rc;
}

static void init_libproc(void) __attribute__((constructor));
static void init_libproc(void){
  have_privs = check_for_privs();
  // ought to count CPUs in /proc/stat instead of relying
  // on glibc, which foolishly tries to parse /proc/cpuinfo
  //
  // SourceForge has an old Alpha running Linux 2.2.20 that
  // appears to have a non-SMP kernel on a 2-way SMP box.
  // _SC_NPROCESSORS_CONF returns 2, resulting in HZ=512
  // _SC_NPROCESSORS_ONLN returns 1, which should work OK
  smp_num_cpus = sysconf(_SC_NPROCESSORS_ONLN);
  if(smp_num_cpus<1) smp_num_cpus=1; /* SPARC glibc is buggy */

    Hertz = find_elf_note(AT_CLKTCK);
    if(Hertz!=NOTE_NOT_FOUND) return;
    fputs("2.4+ kernel w/o ELF notes? -- report this\n", stderr);
    old_Hertz_hack();
}

/***********************************************************************/
//static void loadavg(double *restrict av1, double *restrict av5, double *restrict av15) {
//    double avg_1=0, avg_5=0, avg_15=0;
//    char *restrict savelocale;
//
//    FILE_TO_BUF(LOADAVG_FILE,loadavg_fd);
//    savelocale = setlocale(LC_NUMERIC, NULL);
//    setlocale(LC_NUMERIC, "C");
//    if (sscanf(buf, "%lf %lf %lf", &avg_1, &avg_5, &avg_15) < 3) {
//	fputs("bad data in " LOADAVG_FILE "\n", stderr);
//	exit(1);
//    }
//    setlocale(LC_NUMERIC, savelocale);
//    SET_IF_DESIRED(av1,  avg_1);
//    SET_IF_DESIRED(av5,  avg_5);
//    SET_IF_DESIRED(av15, avg_15);
//}

#define BUFFSIZE (64*1024)

static char buff[BUFFSIZE]; /* used in the procedures */

/***********************************************************************/

static void crash(const char *filename) {
    perror(filename);
    exit(EXIT_FAILURE);
}

/***********************************************************************/

static void getrunners(unsigned int *restrict running, unsigned int *restrict blocked) {
  struct direct *ent;
  DIR *proc;

  *running=0;
  *blocked=0;

  if((proc=opendir("/proc"))==NULL) crash("/proc");

  while(( ent=readdir(proc) )) {
    char tbuf[32];
    char *cp;
    int fd;
    char c;

    if (!isdigit(ent->d_name[0])) continue;
    sprintf(tbuf, "/proc/%s/stat", ent->d_name);

    fd = open(tbuf, O_RDONLY, 0);
    if (fd == -1) continue;
    memset(tbuf, '\0', sizeof tbuf); // didn't feel like checking read()
    read(fd, tbuf, sizeof tbuf - 1); // need 32 byte buffer at most
    close(fd);

    cp = strrchr(tbuf, ')');
    if(!cp) continue;
    c = cp[2];

    if (c=='R') {
      (*running)++;
      continue;
    }
    if (c=='D') {
      (*blocked)++;
      continue;
    }
  }
  closedir(proc);
}

/***********************************************************************/

/* read /proc/vminfo only for 2.5.41 and above */

typedef struct vm_table_struct {
  const char *name;     /* VM statistic name */
  unsigned long *slot;       /* slot in return struct */
} vm_table_struct;

static int compare_vm_table_structs(const void *a, const void *b){
  return strcmp(((const vm_table_struct*)a)->name,((const vm_table_struct*)b)->name);
}

static void vminfo(void){
  char namebuf[16]; /* big enough to hold any row name */
  vm_table_struct findme = { namebuf, NULL};
  vm_table_struct *found;
  char *head;
  char *tail;
  static const vm_table_struct vm_table[] = {
  {"allocstall",          &vm_allocstall},
  {"kswapd_inodesteal",   &vm_kswapd_inodesteal},
  {"kswapd_steal",        &vm_kswapd_steal},
  {"nr_dirty",            &vm_nr_dirty},           // page version of meminfo Dirty
  {"nr_mapped",           &vm_nr_mapped},          // page version of meminfo Mapped
  {"nr_page_table_pages", &vm_nr_page_table_pages},// same as meminfo PageTables
  {"nr_pagecache",        &vm_nr_pagecache},       // gone in 2.5.66+ kernels
  {"nr_reverse_maps",     &vm_nr_reverse_maps},    // page version of meminfo ReverseMaps GONE
  {"nr_slab",             &vm_nr_slab},            // page version of meminfo Slab
  {"nr_unstable",         &vm_nr_unstable},
  {"nr_writeback",        &vm_nr_writeback},       // page version of meminfo Writeback
  {"pageoutrun",          &vm_pageoutrun},
  {"pgactivate",          &vm_pgactivate},
  {"pgalloc",             &vm_pgalloc},  // GONE (now separate dma,high,normal)
  {"pgalloc_dma",         &vm_pgalloc_dma},
  {"pgalloc_high",        &vm_pgalloc_high},
  {"pgalloc_normal",      &vm_pgalloc_normal},
  {"pgdeactivate",        &vm_pgdeactivate},
  {"pgfault",             &vm_pgfault},
  {"pgfree",              &vm_pgfree},
  {"pginodesteal",        &vm_pginodesteal},
  {"pgmajfault",          &vm_pgmajfault},
  {"pgpgin",              &vm_pgpgin},     // important
  {"pgpgout",             &vm_pgpgout},     // important
  {"pgrefill",            &vm_pgrefill},  // GONE (now separate dma,high,normal)
  {"pgrefill_dma",        &vm_pgrefill_dma},
  {"pgrefill_high",       &vm_pgrefill_high},
  {"pgrefill_normal",     &vm_pgrefill_normal},
  {"pgrotated",           &vm_pgrotated},
  {"pgscan",              &vm_pgscan},  // GONE (now separate direct,kswapd and dma,high,normal)
  {"pgscan_direct_dma",   &vm_pgscan_direct_dma},
  {"pgscan_direct_high",  &vm_pgscan_direct_high},
  {"pgscan_direct_normal",&vm_pgscan_direct_normal},
  {"pgscan_kswapd_dma",   &vm_pgscan_kswapd_dma},
  {"pgscan_kswapd_high",  &vm_pgscan_kswapd_high},
  {"pgscan_kswapd_normal",&vm_pgscan_kswapd_normal},
  {"pgsteal",             &vm_pgsteal},  // GONE (now separate dma,high,normal)
  {"pgsteal_dma",         &vm_pgsteal_dma},
  {"pgsteal_high",        &vm_pgsteal_high},
  {"pgsteal_normal",      &vm_pgsteal_normal},
  {"pswpin",              &vm_pswpin},     // important
  {"pswpout",             &vm_pswpout},     // important
  {"slabs_scanned",       &vm_slabs_scanned},
  };
  const int vm_table_count = sizeof(vm_table)/sizeof(vm_table_struct);

  vm_pgalloc = 0;
  vm_pgrefill = 0;
  vm_pgscan = 0;
  vm_pgsteal = 0;

  FILE_TO_BUF(VMINFO_FILE,vminfo_fd);

  head = buf;
  for(;;){
    tail = strchr(head, ' ');
    if(!tail) break;
    *tail = '\0';
    if(strlen(head) >= sizeof(namebuf)){
      head = tail+1;
      goto nextline;
    }
    strcpy(namebuf,head);
    found = bsearch(&findme, vm_table, vm_table_count,
        sizeof(vm_table_struct), compare_vm_table_structs
    );
    head = tail+1;
    if(!found) goto nextline;
    *(found->slot) = strtoul(head,&tail,10);
nextline:

//if(found) fprintf(stderr,"%s=%d\n",found->name,*(found->slot));
//else      fprintf(stderr,"%s not found\n",findme.name);

    tail = strchr(head, '\n');
    if(!tail) break;
    head = tail+1;
  }
  if(!vm_pgalloc)
    vm_pgalloc  = vm_pgalloc_dma + vm_pgalloc_high + vm_pgalloc_normal;
  if(!vm_pgrefill)
    vm_pgrefill = vm_pgrefill_dma + vm_pgrefill_high + vm_pgrefill_normal;
  if(!vm_pgscan)
    vm_pgscan   = vm_pgscan_direct_dma + vm_pgscan_direct_high + vm_pgscan_direct_normal
                + vm_pgscan_kswapd_dma + vm_pgscan_kswapd_high + vm_pgscan_kswapd_normal;
  if(!vm_pgsteal)
    vm_pgsteal  = vm_pgsteal_dma + vm_pgsteal_high + vm_pgsteal_normal;
}

/***********************************************************************/

typedef unsigned long long jiff;

static void getstat(jiff *restrict cuse, jiff *restrict cice, jiff *restrict csys, jiff *restrict cide, jiff *restrict ciow, jiff *restrict cxxx, jiff *restrict cyyy, jiff *restrict czzz,
	     unsigned long *restrict pin, unsigned long *restrict pout, unsigned long *restrict s_in, unsigned long *restrict sout,
	     unsigned *restrict intr, unsigned *restrict ctxt,
	     unsigned int *restrict running, unsigned int *restrict blocked,
	     unsigned int *restrict btime, unsigned int *restrict processes) {
  static int fd;
  unsigned long long llbuf = 0;
  int need_vmstat_file = 0;
  int need_proc_scan = 0;
  const char* b;
  buff[BUFFSIZE-1] = 0;  /* ensure null termination in buffer */

  if(fd){
    lseek(fd, 0L, SEEK_SET);
  }else{
    fd = open("/proc/stat", O_RDONLY, 0);
    if(fd == -1) crash("/proc/stat");
  }
  read(fd,buff,BUFFSIZE-1);
  *intr = 0; 
  *ciow = 0;  /* not separated out until the 2.5.41 kernel */
  *cxxx = 0;  /* not separated out until the 2.6.0-test4 kernel */
  *cyyy = 0;  /* not separated out until the 2.6.0-test4 kernel */
  *czzz = 0;  /* not separated out until the 2.6.11 kernel */

  b = strstr(buff, "cpu ");
  if(b) sscanf(b,  "cpu  %Lu %Lu %Lu %Lu %Lu %Lu %Lu %Lu", cuse, cice, csys, cide, ciow, cxxx, cyyy, czzz);

  b = strstr(buff, "page ");
  if(b) sscanf(b,  "page %lu %lu", pin, pout);
  else need_vmstat_file = 1;

  b = strstr(buff, "swap ");
  if(b) sscanf(b,  "swap %lu %lu", s_in, sout);
  else need_vmstat_file = 1;

  b = strstr(buff, "intr ");
  if(b) sscanf(b,  "intr %Lu", &llbuf);
  *intr = llbuf;

  b = strstr(buff, "ctxt ");
  if(b) sscanf(b,  "ctxt %Lu", &llbuf);
  *ctxt = llbuf;

  b = strstr(buff, "btime ");
  if(b) sscanf(b,  "btime %u", btime);

  b = strstr(buff, "processes ");
  if(b) sscanf(b,  "processes %u", processes);

  b = strstr(buff, "procs_running ");
  if(b) sscanf(b,  "procs_running %u", running);
  else need_proc_scan = 1;

  b = strstr(buff, "procs_blocked ");
  if(b) sscanf(b,  "procs_blocked %u", blocked);
  else need_proc_scan = 1;

  if(need_proc_scan){   /* Linux 2.5.46 (approximately) and below */
    getrunners(running, blocked);
  }

  if(need_vmstat_file){  /* Linux 2.5.40-bk4 and above */
    vminfo();
    *pin  = vm_pgpgin;
    *pout = vm_pgpgout;
    *s_in = vm_pswpin;
    *sout = vm_pswpout;
  }
}

/***********************************************************************/
/*
 * Copyright 1999 by Albert Cahalan; all rights reserved.
 * This file may be used subject to the terms and conditions of the
 * GNU Library General Public License Version 2, or any later version
 * at your option, as published by the Free Software Foundation.
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Library General Public License for more details.
 */

typedef struct mem_table_struct {
  const char *name;     /* memory type name */
  unsigned long *slot; /* slot in return struct */
} mem_table_struct;

static int compare_mem_table_structs(const void *a, const void *b){
  return strcmp(((const mem_table_struct*)a)->name,((const mem_table_struct*)b)->name);
}

/* example data, following junk, with comments added:
 *
 * MemTotal:        61768 kB    old
 * MemFree:          1436 kB    old
 * MemShared:           0 kB    old (now always zero; not calculated)
 * Buffers:          1312 kB    old
 * Cached:          20932 kB    old
 * Active:          12464 kB    new
 * Inact_dirty:      7772 kB    new
 * Inact_clean:      2008 kB    new
 * Inact_target:        0 kB    new
 * Inact_laundry:       0 kB    new, and might be missing too
 * HighTotal:           0 kB
 * HighFree:            0 kB
 * LowTotal:        61768 kB
 * LowFree:          1436 kB
 * SwapTotal:      122580 kB    old
 * SwapFree:        60352 kB    old
 * Inactive:        20420 kB    2.5.41+
 * Dirty:               0 kB    2.5.41+
 * Writeback:           0 kB    2.5.41+
 * Mapped:           9792 kB    2.5.41+
 * Slab:             4564 kB    2.5.41+
 * Committed_AS:     8440 kB    2.5.41+
 * PageTables:        304 kB    2.5.41+
 * ReverseMaps:      5738       2.5.41+
 * SwapCached:          0 kB    2.5.??+
 * HugePages_Total:   220       2.5.??+
 * HugePages_Free:    138       2.5.??+
 * Hugepagesize:     4096 kB    2.5.??+
 */


static void meminfo(void){
  char namebuf[16]; /* big enough to hold any row name */
  mem_table_struct findme = { namebuf, NULL};
  mem_table_struct *found;
  char *head;
  char *tail;
  static const mem_table_struct mem_table[] = {
  {"Active",       &kb_active},       // important
  {"AnonPages",    &kb_anon_pages},
  {"Bounce",       &kb_bounce},
  {"Buffers",      &kb_main_buffers}, // important
  {"Cached",       &kb_main_cached},  // important
  {"CommitLimit",  &kb_commit_limit},
  {"Committed_AS", &kb_committed_as},
  {"Dirty",        &kb_dirty},        // kB version of vmstat nr_dirty
  {"HighFree",     &kb_high_free},
  {"HighTotal",    &kb_high_total},
  {"Inact_clean",  &kb_inact_clean},
  {"Inact_dirty",  &kb_inact_dirty},
  {"Inact_laundry",&kb_inact_laundry},
  {"Inact_target", &kb_inact_target},
  {"Inactive",     &kb_inactive},     // important
  {"LowFree",      &kb_low_free},
  {"LowTotal",     &kb_low_total},
  {"Mapped",       &kb_mapped},       // kB version of vmstat nr_mapped
  {"MemFree",      &kb_main_free},    // important
  {"MemShared",    &kb_main_shared},  // important, but now gone!
  {"MemTotal",     &kb_main_total},   // important
  {"NFS_Unstable", &kb_nfs_unstable},
  {"PageTables",   &kb_pagetables},   // kB version of vmstat nr_page_table_pages
  {"ReverseMaps",  &nr_reversemaps},  // same as vmstat nr_page_table_pages
  {"SReclaimable", &kb_swap_reclaimable}, // "swap reclaimable" (dentry and inode structures)
  {"SUnreclaim",   &kb_swap_unreclaimable},
  {"Slab",         &kb_slab},         // kB version of vmstat nr_slab
  {"SwapCached",   &kb_swap_cached},
  {"SwapFree",     &kb_swap_free},    // important
  {"SwapTotal",    &kb_swap_total},   // important
  {"VmallocChunk", &kb_vmalloc_chunk},
  {"VmallocTotal", &kb_vmalloc_total},
  {"VmallocUsed",  &kb_vmalloc_used},
  {"Writeback",    &kb_writeback},    // kB version of vmstat nr_writeback
  };
  const int mem_table_count = sizeof(mem_table)/sizeof(mem_table_struct);

  FILE_TO_BUF(MEMINFO_FILE,meminfo_fd);

  kb_inactive = ~0UL;

  head = buf;
  for(;;){
    tail = strchr(head, ':');
    if(!tail) break;
    *tail = '\0';
    if(strlen(head) >= sizeof(namebuf)){
      head = tail+1;
      goto nextline;
    }
    strcpy(namebuf,head);
    found = bsearch(&findme, mem_table, mem_table_count,
        sizeof(mem_table_struct), compare_mem_table_structs
    );
    head = tail+1;
    if(!found) goto nextline;
    *(found->slot) = strtoul(head,&tail,10);
nextline:
    tail = strchr(head, '\n');
    if(!tail) break;
    head = tail+1;
  }
  if(!kb_low_total){  /* low==main except with large-memory support */
    kb_low_total = kb_main_total;
    kb_low_free  = kb_main_free;
  }
  if(kb_inactive==~0UL){
    kb_inactive = kb_inact_dirty + kb_inact_clean + kb_inact_laundry;
  }
  kb_swap_used = kb_swap_total - kb_swap_free;
  kb_main_used = kb_main_total - kb_main_free;
}

/*****************************************************************/


/////////////////////////////////////////////////////////////////////////
//// based on Fabian Frederick's /proc/diskstats parser
//
//
//static unsigned int getpartitions_num(struct disk_stat *disks, int ndisks){
//  int i=0;
//  int partitions=0;
//
//  for (i=0;i<ndisks;i++){
//	partitions+=disks[i].partitions;
//  }
//  return partitions;
//
//}
//
///////////////////////////////////////////////////////////////////////////////

//static unsigned int getdiskstat(struct disk_stat **disks, struct partition_stat **partitions){
//  FILE* fd;
//  int cDisk = 0;
//  int cPartition = 0;
//  int fields;
//  unsigned dummy;
//
//  *disks = NULL;
//  *partitions = NULL;
//  buff[BUFFSIZE-1] = 0; 
//  fd = fopen("/proc/diskstats", "rb");
//  if(!fd) crash("/proc/diskstats");
//
//  for (;;) {
//    if (!fgets(buff,BUFFSIZE-1,fd)){
//      fclose(fd);
//      break;
//    }
//    fields = sscanf(buff, " %*d %*d %*s %*u %*u %*u %*u %*u %*u %*u %*u %*u %*u %u", &dummy);
//    if (fields == 1){
//      (*disks) = realloc(*disks, (cDisk+1)*sizeof(struct disk_stat));
//      sscanf(buff,  "   %*d    %*d %15s %u %u %llu %u %u %u %llu %u %u %u %u",
//        //&disk_major,
//        //&disk_minor,
//        (*disks)[cDisk].disk_name,
//        &(*disks)[cDisk].reads,
//        &(*disks)[cDisk].merged_reads,
//        &(*disks)[cDisk].reads_sectors,
//        &(*disks)[cDisk].milli_reading,
//        &(*disks)[cDisk].writes,
//        &(*disks)[cDisk].merged_writes,
//        &(*disks)[cDisk].written_sectors,
//        &(*disks)[cDisk].milli_writing,
//        &(*disks)[cDisk].inprogress_IO,
//        &(*disks)[cDisk].milli_spent_IO,
//        &(*disks)[cDisk].weighted_milli_spent_IO
//      );
//        (*disks)[cDisk].partitions=0;
//      cDisk++;
//    }else{
//      (*partitions) = realloc(*partitions, (cPartition+1)*sizeof(struct partition_stat));
//      fflush(stdout);
//      sscanf(buff,  "   %*d    %*d %15s %u %llu %u %u",
//        //&part_major,
//        //&part_minor,
//        (*partitions)[cPartition].partition_name,
//        &(*partitions)[cPartition].reads,
//        &(*partitions)[cPartition].reads_sectors,
//        &(*partitions)[cPartition].writes,
//        &(*partitions)[cPartition].requested_writes
//      );
//      (*partitions)[cPartition++].parent_disk = cDisk-1;
//      (*disks)[cDisk-1].partitions++;	
//    }
//  }
//
//  return cDisk;
//}

/////////////////////////////////////////////////////////////////////////////
// based on Fabian Frederick's /proc/slabinfo parser

//static unsigned int getslabinfo (struct slab_cache **slab){
//  FILE* fd;
//  int cSlab = 0;
//  buff[BUFFSIZE-1] = 0; 
//  *slab = NULL;
//  fd = fopen("/proc/slabinfo", "rb");
//  if(!fd) crash("/proc/slabinfo");
//  while (fgets(buff,BUFFSIZE-1,fd)){
//    if(!memcmp("slabinfo - version:",buff,19)) continue; // skip header
//    if(*buff == '#')                           continue; // skip comments
//    (*slab) = realloc(*slab, (cSlab+1)*sizeof(struct slab_cache));
//    sscanf(buff,  "%47s %u %u %u %u",  // allow 47; max seen is 24
//      (*slab)[cSlab].name,
//      &(*slab)[cSlab].active_objs,
//      &(*slab)[cSlab].num_objs,
//      &(*slab)[cSlab].objsize,
//      &(*slab)[cSlab].objperslab
//    ) ;
//    cSlab++;
//  }
//  fclose(fd);
//  return cSlab;
//}
//
/////////////////////////////////////////////////////////////////////////////

//unsigned get_pid_digits(void){
//  char pidbuf[24];
//  char *endp;
//  long rc;
//  int fd;
//  static unsigned ret;
//
//  if(ret) goto out;
//  ret = 5;
//  fd = open("/proc/sys/kernel/pid_max", O_RDONLY);
//  if(fd==-1) goto out;
//  rc = read(fd, pidbuf, sizeof pidbuf);
//  close(fd);
//  if(rc<3) goto out;
//  pidbuf[rc] = '\0';
//  rc = strtol(pidbuf,&endp,10);
//  if(rc<42) goto out;
//  if(*endp && *endp!='\n') goto out;
//  rc--;  // the pid_max value is really the max PID plus 1
//  ret = 0;
//  while(rc){
//    rc /= 10;
//    ret++;
//  }
//out:
//  return ret;
//}

///////////////////////////////////////////////////////////////////////

static void stat2proc(const char* S, proc_t *restrict P) {
    unsigned num;
    char* tmp;

//ENTER(0x160);

    /* fill in default values for older kernels */
    P->processor = 0;
    P->rtprio = -1;
    P->sched = -1;
    P->nlwp = 0;

    S = strchr(S, '(') + 1;
    tmp = strrchr(S, ')');
    num = tmp - S;
    if(unlikely(num >= sizeof P->cmd)) num = sizeof P->cmd - 1;
    memcpy(P->cmd, S, num);
    P->cmd[num] = '\0';
    S = tmp + 2;                 // skip ") "

    num = sscanf(S,
       "%c "
       "%d %d %d %d %d "
       "%lu %lu %lu %lu %lu "
       "%Lu %Lu %Lu %Lu "  /* utime stime cutime cstime */
       "%ld %ld "
       "%d "
       "%ld "
       "%Lu "  /* start_time */
       "%lu "
       "%ld "
       "%lu %"KLF"u %"KLF"u %"KLF"u %"KLF"u %"KLF"u "
       "%*s %*s %*s %*s " /* discard, no RT signals & Linux 2.1 used hex */
       "%"KLF"u %*lu %*lu "
       "%d %d "
       "%lu %lu",
       &P->state,
       &P->ppid, &P->pgrp, &P->session, &P->tty, &P->tpgid,
       &P->flags, &P->min_flt, &P->cmin_flt, &P->maj_flt, &P->cmaj_flt,
       &P->utime, &P->stime, &P->cutime, &P->cstime,
       &P->priority, &P->nice,
       &P->nlwp,
       &P->alarm,
       &P->start_time,
       &P->vsize,
       &P->rss,
       &P->rss_rlim, &P->start_code, &P->end_code, &P->start_stack, &P->kstk_esp, &P->kstk_eip,
/*     P->signal, P->blocked, P->sigignore, P->sigcatch,   */ /* can't use */
       &P->wchan, /* &P->nswap, &P->cnswap, */  /* nswap and cnswap dead for 2.4.xx and up */
/* -- Linux 2.0.35 ends here -- */
       &P->exit_signal, &P->processor,  /* 2.2.1 ends with "exit_signal" */
/* -- Linux 2.2.8 to 2.5.17 end here -- */
       &P->rtprio, &P->sched  /* both added to 2.5.18 */
    );

    if(!P->nlwp){
      P->nlwp = 1;
    }
	P->pcpu = 0;

//LEAVE(0x160);
}

/////////////////////////////////////////////////////////////////////////

static void statm2proc(const char* s, proc_t *restrict P) {
    int num;
    num = sscanf(s, "%ld %ld %ld %ld %ld %ld %ld",
	   &P->size, &P->resident, &P->share,
	   &P->trs, &P->lrs, &P->drs, &P->dt);
/*    fprintf(stderr, "statm2proc converted %d fields.\n",num); */
}

static int file2str(const char *directory, const char *what, char *ret, int cap) {
    static char filename[80];
    int fd, num_read;

    sprintf(filename, "%s/%s", directory, what);
    fd = open(filename, O_RDONLY, 0);
    if(unlikely(fd==-1)) return -1;
    num_read = read(fd, ret, cap - 1);
    close(fd);
    if(unlikely(num_read<=0)) return -1;
    ret[num_read] = '\0';
    return num_read;
}

/////////////////////////////////////////////////////////////////////////

static pid_t pid = 0;
static char pid_path[64];

static proc_t* simple_readproc(proc_t *restrict const p) {
    static char sbuf[1024];	// buffer for stat,statm
	if (unlikely(file2str(pid_path, "stat", sbuf, sizeof sbuf) == -1)) return NULL;
	stat2proc(sbuf, p);				/* parse /proc/#/stat */
	if (likely(file2str(pid_path, "statm", sbuf, sizeof sbuf) != -1 )) {
		statm2proc(sbuf, p);
	}
	return p;
}

typedef unsigned long long TIC_t;
static float     Frame_tscale;          // so we can '*' vs. '/' WHEN 'pcpu'

static void prochlp (proc_t *this)
{
   static TIC_t prev_tics;
   TIC_t new_tics;
   TIC_t tics_diff;
   static struct timeval oldtimev;
   struct timeval timev;
   struct timezone timez;
   float et;

   gettimeofday(&timev, &timez);
   et = (timev.tv_sec - oldtimev.tv_sec)
      + (float)(timev.tv_usec - oldtimev.tv_usec) / 1000000.0;
   oldtimev.tv_sec = timev.tv_sec;
   oldtimev.tv_usec = timev.tv_usec;

// if in Solaris mode, adjust our scaling for all cpus
//   Frame_tscale = 100.0f / ((float)Hertz * (float)et * (Rc.mode_irixps ? 1 : Cpu_tot));
   Frame_tscale = 100.0f / ((float)Hertz * (float)et);
   if(!this) return;

   /* calculate time in this process; the sum of user time (utime) and
      system time (stime) -- but PLEASE dont waste time and effort on
      calcs and saves that go unused, like the old top! */
   new_tics = tics_diff = (this->utime + this->stime);
   tics_diff -= prev_tics;
   prev_tics = new_tics;

   // we're just saving elapsed tics, to be converted into %cpu if
   // this task wins it's displayable screen row lottery... */
   this->pcpu = tics_diff;
   return;
}

///////////////////////////////////////////////////////////////////////////

static unsigned long dataUnit=1024;

static unsigned long unitConvert(unsigned int size){
 float cvSize;
 cvSize=(float)size/dataUnit*1024;
 return ((unsigned long) cvSize);
}

static unsigned long long getTime(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

#define KeyValue_u(K,V)    LOG_u, (K), ((uintptr_t)V)
#define KeyValue_s(K,V)    LOG_s, (K), (V)
#define KeyValue_p(K,V)    LOG_u, (K), (V)
#define KeyValue_f(K,V)    LOG_f, (K), ((float)V)

static unsigned Page_size;
static unsigned page_to_kb_shift;

#define PAGES_TO_KB(n)  (unsigned long)( (n) << page_to_kb_shift )

static void _monitorResource(void) {
	unsigned int hz = Hertz;
	unsigned int running,blocked,dummy_1,dummy_2;
	jiff cpu_use[2], cpu_nic[2], cpu_sys[2], cpu_idl[2], cpu_iow[2], cpu_xxx[2], cpu_yyy[2], cpu_zzz[2];
	jiff duse, dsys, didl, diow, dstl, Div, divo2;
	unsigned long pgpgin[2], pgpgout[2], pswpin[2], pswpout[2];
	unsigned int intr[2], ctxt[2];
	unsigned long kb_per_page = sysconf(_SC_PAGESIZE) / 1024ul;

	meminfo();

	getstat(cpu_use,cpu_nic,cpu_sys,cpu_idl,cpu_iow,cpu_xxx,cpu_yyy,cpu_zzz,
			pgpgin,pgpgout,pswpin,pswpout,
			intr,ctxt,
			&running,&blocked,
			&dummy_1, &dummy_2);

	duse= *cpu_use + *cpu_nic; 
	dsys= *cpu_sys + *cpu_xxx + *cpu_yyy;
	didl= *cpu_idl;
	diow= *cpu_iow;
	dstl= *cpu_zzz;
	Div= duse+dsys+didl+diow+dstl;
	divo2= Div/2UL;

	proc_t *p = (proc_t *)alloca(sizeof(proc_t));
	p = simple_readproc(p);
	prochlp(p);
	static void *arg;

	trace(arg,
			KeyValue_u("pid",           pid),
			KeyValue_u("time",          getTime()),
			KeyValue_u("procs_running", running),
			KeyValue_u("procs_blocked", blocked),
			KeyValue_u("memory_swpd",   unitConvert(kb_swap_used)),
			KeyValue_u("memory_free",   unitConvert(kb_main_free)),
			KeyValue_u("memory_buff",   unitConvert(kb_main_buffers)),
			KeyValue_u("memory_cache",  unitConvert(kb_main_cached)),
			KeyValue_u("swap_si",       (unsigned)( (*pswpin  * unitConvert(kb_per_page) * hz + divo2) / Div )),
			KeyValue_u("swap_so",       (unsigned)( (*pswpout  * unitConvert(kb_per_page) * hz + divo2) / Div )),
			KeyValue_u("io_bi",         (unsigned)( (*pgpgin                * hz + divo2) / Div )),
			KeyValue_u("io_bo",         (unsigned)( (*pgpgout               * hz + divo2) / Div )),
			KeyValue_u("system_in",     (unsigned)( (*intr                  * hz + divo2) / Div )),
			KeyValue_u("system_cs",     (unsigned)( (*ctxt                  * hz + divo2) / Div )),
			KeyValue_u("cpu_us",        (unsigned)( (100*duse                    + divo2) / Div )),
			KeyValue_u("cpu_sy",        (unsigned)( (100*dsys                    + divo2) / Div )),
			KeyValue_u("cpu_id",        (unsigned)( (100*didl                    + divo2) / Div )),
			KeyValue_u("cpu_wa",        (unsigned)( (100*diow                    + divo2) / Div )),
			KeyValue_u("cpu_usage",     (unsigned)( (float)p->pcpu * Frame_tscale)), // TODO to float
			KeyValue_u("mem_usage",     (unsigned)( (float)PAGES_TO_KB(p->resident) * 100 / kb_main_total)) // TODO to float
				);
	return;
}

// -------------------------------------------------------------------------

static void *monitor_func(void *arg)
{
	while(true) {
		_monitorResource();
		sleep(1);
	}
	return NULL;
}

static struct option long_options2[] = {
	/* These options set a flag. */
	{"verbose", no_argument,       &verbose_debug, 1},
	{"verbose:gc",    no_argument, &verbose_gc, 1},
	{"verbose:sugar", no_argument, &verbose_sugar, 1},
	{"verbose:code",  no_argument, &verbose_code, 1},
	{"interactive", no_argument,   0, 'i'},
	{"typecheck",   no_argument,   0, 'c'},
	{"define",    required_argument, 0, 'D'},
	{"import",    required_argument, 0, 'I'},
	{"startwith", required_argument, 0, 'S'},
	{"test",  required_argument, 0, 'T'},
	{"test-with",  required_argument, 0, 'T'},
	{"builtin-test",  required_argument, 0, 'B'},
	{NULL, 0, 0, 0},
};

static int konoha_parseopt(KonohaContext* konoha, PlatformApiVar *plat, int argc, char **argv)
{
	kbool_t ret = true;
	int scriptidx = 0;
	while (1) {
		int option_index = 0;
		int c = getopt_long (argc, argv, "icD:I:S:L:", long_options2, &option_index);
		if (c == -1) break; /* Detect the end of the options. */
		switch (c) {
		case 0:
			/* If this option set a flag, do nothing else now. */
			if (long_options2[option_index].flag != 0)
				break;
			printf ("option %s", long_options2[option_index].name);
			if (optarg)
				printf (" with arg %s", optarg);
			printf ("\n");
			break;

		case 'c': {
			compileonly_flag = 1;
			KonohaContext_setCompileOnly(konoha);
		}
		break;

		case 'i': {
			interactive_flag = 1;
			KonohaContext_setInteractive(konoha);
		}
		break;

		case 'B':
			return CommandLine_doBuiltInTest(konoha, optarg);

		case 'D':
			CommandLine_define(konoha, optarg);
			break;

		case 'I':
			CommandLine_import(konoha, optarg);
			break;

		case 'S':
			konoha_startup(konoha, optarg);
			break;

		case 'T':
//			DUMP_P ("option --test-with `%s'\n", optarg);
			verbose_debug = 0;
			verbose_sugar = 0;
			verbose_gc    = 0;
			verbose_code  = 0;
			plat->debugPrintf = NOP_debugPrintf;
			plat->printf_i  = TEST_printf;
			plat->vprintf_i = TEST_vprintf;
			plat->beginTag  = TEST_begin;
			plat->endTag    = TEST_end;
			plat->shortText = TEST_shortText;
			plat->reportCaughtException = TEST_reportCaughtException;
			return KonohaContext_test(konoha, optarg);

		case '?':
			/* getopt_long already printed an error message. */
			break;

		default:
			return 1;
		}
	}
	scriptidx = optind;
	CommandLine_setARGV(konoha, argc - scriptidx, argv + scriptidx);

	pid = getpid();
	snprintf(pid_path, 64, "/proc/%d", pid);
	Page_size = getpagesize(); // for mem usage
	int i = Page_size;
	while(i > 1024) {
		i >>= 1;
		page_to_kb_shift++;
	}
	openlog("loggerkonoha", LOG_PID, LOG_LOCAL7); // for using syslog
	pthread_t logging_thread;
	pthread_create(&logging_thread, NULL, monitor_func, (void *)konoha);

	if(scriptidx < argc) {
		ret = konoha_load(konoha, argv[scriptidx]);
	}
	else {
		interactive_flag = 1;
		KonohaContext_setInteractive(konoha);
	}
	if(ret && interactive_flag) {
		CommandLine_import(konoha, "konoha.i");
		ret = konoha_shell(konoha);
	}
	closelog();
	return (ret == true) ? 0 : 1;
}

// -------------------------------------------------------------------------
// ** main **

int main(int argc, char *argv[])
{
	kbool_t ret = 1;
	if(getenv("KONOHA_DEBUG") != NULL) {
		verbose_debug = 1;
		verbose_gc = 1;
		verbose_sugar = 1;
		verbose_code = 1;
	}
	PlatformApi *logger_platform = KonohaUtils_getDefaultPlatformApi();
	PlatformApiVar *logger_platformVar = (PlatformApiVar *)logger_platform;
	logger_platformVar->monitorResource = _monitorResource;
	KonohaContext* konoha = konoha_open(logger_platform);
	ret = konoha_parseopt(konoha, logger_platformVar, argc, argv);
	konoha_close(konoha);
	return ret ? konoha_detectFailedAssert: 0;
}

#ifdef __cplusplus
}
#endif

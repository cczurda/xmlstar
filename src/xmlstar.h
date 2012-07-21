#ifndef XMLSTAR_H
#define XMLSTAR_H

#include <config.h>
#include <stdlib.h>

#if HAVE_SETMODE && HAVE_DECL_O_BINARY
# include <io.h>
# include <fcntl.h>
# define set_stdout_binary() setmode(1, O_BINARY)
#else
# define set_stdout_binary()
#endif

#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>
#include <libxml/xmlreader.h>

typedef enum { /* EXIT_SUCCESS = 0, EXIT_FAILURE = 1, */
    EXIT_BAD_ARGS = EXIT_FAILURE+1, EXIT_BAD_FILE,
    EXIT_LIB_ERROR, EXIT_INTERNAL_ERROR } exit_status;

#define COUNT_OF(array) (sizeof(array)/sizeof(*array))

typedef enum { QUIET, VERBOSE } Verbosity;

typedef struct _errorInfo {
    const char *filename; /* file error occured in, if any, else NULL */
    xmlTextReaderPtr xmlReader;
    Verbosity verbose;
} ErrorInfo;

void reportError(void *ptr, xmlErrorPtr error);

void registerXstarVariable(xmlXPathContextPtr ctxt,
    const char* name, xmlXPathObjectPtr value);
void registerXstarNs(xmlXPathContextPtr ctxt);

void cleanupNSArr(xmlChar **ns_arr);
extern xmlChar *ns_arr[];

extern int edMain(void);
extern int selMain(void);
extern int trMain(void);
extern int valMain(void);
extern int foMain(void);
extern int elMain(void);
extern int c14nMain(void);
extern int lsMain(void);
extern int pyxMain(void);
extern int depyxMain(void);
extern int escMain(int escape);

typedef enum { ARG0, ARG_PEEK, ARG_NEXT, OPTION_NEXT } ArgOp;
extern int handle_namespace_args;
extern int main_argc;
extern char** main_argv;
extern int main_argi;
char* get_arg(ArgOp op);

#endif  /* XMLSTAR_H */

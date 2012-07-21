/*  $Id: xml_select.c,v 1.67 2005/01/07 02:02:13 mgrouch Exp $  */

/*

XMLStarlet: Command Line Toolkit to query/edit/check/transform XML documents

Copyright (c) 2002-2004 Mikhail Grushinskiy.  All Rights Reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.

*/

#include <config.h>

#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <ctype.h>

#include <libxml/tree.h>
#include <libxslt/templates.h>

#include "xmlstar.h"
#include "trans.h"

/* max length of xmlstarlet supplied (ie not from command line) namespaces
 * currently xalanredirect is longest, at 13 characters*/
#define MAX_NS_PREFIX_LEN 20

typedef struct {
    const xmlChar *href, *prefix;
} NsEntry;

static const NsEntry ns_entries[] = {
    { BAD_CAST "http://exslt.org/common", BAD_CAST "exslt" },
    { BAD_CAST "http://exslt.org/math", BAD_CAST "math" },
    { BAD_CAST "http://exslt.org/dates-and-times", BAD_CAST "date" },
    { BAD_CAST "http://exslt.org/functions", BAD_CAST "func" },
    { BAD_CAST "http://exslt.org/sets", BAD_CAST "set" },
    { BAD_CAST "http://exslt.org/strings", BAD_CAST "str" },
    { BAD_CAST "http://exslt.org/dynamic", BAD_CAST "dyn" },
    { BAD_CAST "http://icl.com/saxon", BAD_CAST "saxon" },
    { BAD_CAST "org.apache.xalan.xslt.extensions.Redirect",
      BAD_CAST "xalanredirect"}, /* see MAX_NS_PREFIX_LEN */
    { BAD_CAST "http://www.jclark.com/xt", BAD_CAST "xt" },
    { BAD_CAST "http://xmlsoft.org/XSLT/namespace", BAD_CAST "libxslt" },
    { BAD_CAST "http://xmlsoft.org/XSLT/", BAD_CAST "test" },
};

static const NsEntry*
lookup_ns_entry(const char *prefix, int len) {
    int i;
    for (i = 0; i < COUNT_OF(ns_entries); i++) {
        if (xmlStrncmp(BAD_CAST prefix, ns_entries[i].prefix, len) == 0)
            return &ns_entries[i];
    }
    return NULL;
}


typedef struct _selOptions {
    int quiet;            /* No output */
    int printXSLT;        /* Display prepared XSLT */
    int printRoot;        /* Print root element in output (if XML) */
    int outText;          /* Output is text */
    int indent;           /* Indent output */
    int noblanks;         /* Remove insignificant spaces from XML tree */
    int no_omit_decl;     /* Print XML declaration line <?xml version="1.0"?> */
    int nonet;            /* refuse to fetch DTDs or entities over network */
    const xmlChar *encoding; /* the "encoding" attribute on the stylesheet's <xsl:output/> */
} selOptions;

typedef selOptions *selOptionsPtr;

typedef enum { TARG_NONE = 0, TARG_SORT_OP, TARG_XPATH,
               TARG_ATTR_STRING, TARG_STRING, TARG_VAR,
               /* template args below don't consume any command line args */
               TARG_NEWLINE, TARG_NO_CMDLINE = TARG_NEWLINE,
               TARG_INP_NAME, TARG_STR_NAME_SELECT
} template_argument_type;
typedef struct {
    const xmlChar *attrname;
    template_argument_type type;
} template_option_argument;

#define TEMPLATE_OPT_MAX_ARGS 2

typedef struct {
    char shortopt;
    const char *longopt;
    const xmlChar *xslname;
    template_option_argument arguments[TEMPLATE_OPT_MAX_ARGS];
    int nest;
} template_option;

static const template_option
    OPT_TEMPLATE = { 't', "template" },
    OPT_COPY_OF  = { 'c', "copy-of", BAD_CAST "copy-of", {{BAD_CAST "select", TARG_XPATH}}, 0 },
    OPT_VALUE_OF = { 'v', "value-of", BAD_CAST "with-param", {{BAD_CAST "name", TARG_STR_NAME_SELECT},
                                                              {BAD_CAST "select", TARG_XPATH}}, -1 },
    OPT_OUTPUT   = { 'o', "output", BAD_CAST "text", {{NULL, TARG_STRING}}, 0 },
    OPT_NL       = { 'n', "nl", BAD_CAST "value-of", {{NULL, TARG_NEWLINE}}, 0 },
    OPT_INP_NAME = { 'f', "inp-name", BAD_CAST "copy-of", {{NULL, TARG_INP_NAME}}, 0 },
    OPT_MATCH    = { 'm', "match", BAD_CAST "for-each", {{BAD_CAST "select", TARG_XPATH}}, 1 },
    OPT_IF       = { 'i', "if", BAD_CAST"when", {{BAD_CAST "test", TARG_XPATH}}, 1 },
    OPT_ELIF     = { 0,   "elif", BAD_CAST "when", {{BAD_CAST "test", TARG_XPATH}}, 1 },
    OPT_ELSE     = { 0,   "else", BAD_CAST "otherwise", {{NULL}}, 1 },
    OPT_ELEM     = { 'e', "elem", BAD_CAST "element", {{BAD_CAST "name", TARG_ATTR_STRING}}, 1 },
    OPT_ATTR     = { 'a', "attr", BAD_CAST "attribute", {{BAD_CAST "name", TARG_ATTR_STRING}}, 1 },
    OPT_BREAK    = { 'b', "break", NULL, {{NULL}}, -1 },
    OPT_SORT     = { 's', "sort", BAD_CAST "sort", {{NULL, TARG_SORT_OP}, {BAD_CAST "select", TARG_XPATH}}, 0 },
    OPT_VAR      = { 0,   "var", BAD_CAST "variable", {{BAD_CAST "name", TARG_VAR}}, 1},

    *TEMPLATE_OPTIONS[] = {
        &OPT_TEMPLATE,
        &OPT_COPY_OF,
        &OPT_VALUE_OF,
        &OPT_OUTPUT,
        &OPT_NL,
        &OPT_INP_NAME,
        &OPT_MATCH,
        &OPT_IF,
        &OPT_ELIF,
        &OPT_ELSE,
        &OPT_ELEM,
        &OPT_ATTR,
        &OPT_BREAK,
        &OPT_SORT,
        &OPT_VAR
    };

void
caseSortFunction(xsltTransformContextPtr ctxt, xmlNodePtr *sorts,
    int nbsorts);

/**
 *  Print small help for command line options
 */
void
selUsage(exit_status status)
{
    extern void fprint_select_usage(FILE* out, const char* argv0);
    extern const char more_info[];
    extern const char libxslt_more_info[];
    FILE *o = (status == EXIT_SUCCESS)? stdout : stderr;
    fprint_select_usage(o, get_arg(ARG0));
    fprintf(o, "%s", more_info);
    fprintf(o, "%s", libxslt_more_info);
    exit(status);
}

/**
 *  Initialize global command line options
 */
void
selInitOptions(selOptionsPtr ops)
{
    ops->quiet = 0;
    ops->printXSLT = 0;
    ops->printRoot = 0;
    ops->outText = 0;
    ops->indent = 0;
    ops->noblanks = 0;
    ops->no_omit_decl = 0;
    ops->nonet = 1;
    ops->encoding = NULL;
}

/**
 *  Parse global command line options
 */
void
selParseOptions(selOptionsPtr ops)
{
    for (;;) {
        const char* arg = get_arg(ARG_PEEK);
        if (arg[0] != '-' || strcmp(arg, "-t") == 0 || strcmp(arg, "--template") == 0)
            break;
        get_arg(ARG_NEXT);

        if (strcmp(arg, "-C") == 0) {
            ops->printXSLT = 1;
        } else if (strcmp(arg, "-Q") == 0 || strcmp(arg, "--quiet") == 0) {
            ops->quiet = 1;
        } else if (strcmp(arg, "-B") == 0 || strcmp(arg, "--noblanks") == 0) {
            ops->noblanks = 1;
        } else if (strcmp(arg, "-T") == 0 || strcmp(arg, "--text") == 0) {
            ops->outText = 1;
        } else if (strcmp(arg, "-R") == 0 || strcmp(arg, "--root") == 0) {
            ops->printRoot = 1;
        } else if (strcmp(arg, "-I") == 0 || strcmp(arg, "--indent") == 0) {
            ops->indent = 1;
        } else if (strcmp(arg, "-D") == 0 || strcmp(arg, "--xml-decl") == 0) {
            ops->no_omit_decl = 1;
        } else if (strcmp(arg, "-E") == 0 || strcmp(arg, "--encode") == 0) {
            const char* encoding = get_arg(ARG_NEXT);
            if (!encoding || encoding[0] == '-') {
                fprintf(stderr, "-E option requires argument <encoding> ex: (utf-8, unicode...)\n");
                exit(EXIT_BAD_ARGS);
            } else {
                ops->encoding = BAD_CAST encoding;
            }
        } else if (strcmp(arg, "--net") == 0) {
            ops->nonet = 0;
        } else if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0 ||
                 strcmp(arg, "-?") == 0 || strcmp(arg, "-Z") == 0) {
            selUsage(EXIT_SUCCESS);
        }
    }
}


/**
 * Search for namespace references in @xpath and add them to @root. Note that we
 * might pickup things that aren't actually namespace references because we
 * don't have a full XPath parser. That's okay, an extra namespace definition
 * won't hurt anyone.
 */
static void
checkNsRefs(xmlNodePtr root, const char *xpath)
{
    const char *colon;
    for (colon = xpath; colon; colon++) {
        int ns_idx = -1;

        colon = strchr(colon, ':');
        if (!colon) break;

        for (;; ns_idx--) {
            if (&colon[ns_idx] < xpath
                ||!isalnum(colon[ns_idx])) {
                const NsEntry *ns;
                ns_idx++;
                ns = lookup_ns_entry(&colon[ns_idx], -ns_idx);
                if (ns) xmlNewNs(root, ns->href, ns->prefix);
                break;
            }
            if (-ns_idx >= MAX_NS_PREFIX_LEN) break;
        }
    }
}

/**
 *  Prepare XSLT template based on command line options
 *  starting one argument after the "-t" or "--template"
 *
 *  @returns 1 if there is following template, 0 otherwise.
 */
int
selGenTemplate(xmlNodePtr root, xmlNodePtr template_node,
    xmlNsPtr xslns, selOptionsPtr ops, int* use_inputfile, int* use_value_of)
{
    int have_next_template = 0;
    int templateEmpty = 1;
    const template_option *targ = NULL;
    xmlNodePtr node = template_node;

    for (;;) {
        xmlNodePtr newnode = NULL;
        const template_option *newtarg = NULL;
        int j;
        int nesting;
        const char* option = get_arg(OPTION_NEXT);

        if (!option) break;
        for (j = 0; j < sizeof(TEMPLATE_OPTIONS)/sizeof(*TEMPLATE_OPTIONS); j++) {
            newtarg = TEMPLATE_OPTIONS[j];
            if (option[1] == '-' && strcmp(newtarg->longopt, &option[2]) == 0)
                goto found_option; /* long option */
            else if(newtarg->shortopt == option[1])
                goto found_option; /* short option */
        }
        fprintf(stderr, "unrecognized option: %s\n", option);
        exit(EXIT_BAD_ARGS);

    found_option:
        if (newtarg == &OPT_SORT && (targ != &OPT_MATCH && targ != &OPT_SORT)) {
            fprintf(stderr, "sort(s) must follow match\n");
            exit(EXIT_BAD_ARGS);
        } else if (newtarg == &OPT_TEMPLATE) {
            have_next_template = 1;
            break;
        } else if (newtarg == &OPT_IF) {
            node = xmlNewChild(node, xslns, BAD_CAST "choose", NULL);
            node->_private = (void*) &OPT_IF;
        } else if (newtarg == &OPT_ELIF || newtarg == &OPT_ELSE) {
            node = node->parent;
            if (node->_private != &OPT_IF) {
                fprintf(stderr, "else without if\n");
                exit(EXIT_BAD_ARGS);
            }
        } else if (newtarg == &OPT_VALUE_OF) {
            node = xmlNewChild(node, xslns, BAD_CAST "call-template", NULL);
            xmlNewProp(node, BAD_CAST "name", BAD_CAST "value-of-template");
            node->_private = (void*) &OPT_VALUE_OF;
            *use_value_of = 1;
            /* value-of-template uses exslt:node-set */
            checkNsRefs(root, "exslt:node-set");
        }

        templateEmpty = 0;
        nesting = newtarg->nest;

        if (newtarg->xslname)
            newnode = xmlNewChild(node, xslns, newtarg->xslname, NULL);

        for (j = 0; j < TEMPLATE_OPT_MAX_ARGS && newtarg->arguments[j].type; j++) {
            const char* arg = NULL;
            if (newtarg->arguments[j].type < TARG_NO_CMDLINE) {
                arg = get_arg(ARG_NEXT);
                if (!arg) {
                    fprintf(stderr, "%s expected another argument\n", option);
                    exit(EXIT_BAD_ARGS);
                }
            }
            switch (newtarg->arguments[j].type) {
            case TARG_VAR: {
                char *equals = strchr(arg, '=');
                if (equals) {
                    *equals = '\0';
                    xmlNewProp(newnode, BAD_CAST "select", BAD_CAST (&equals[1]));
                    nesting = 0;
                }
                xmlNewProp(newnode, newtarg->arguments[j].attrname, BAD_CAST arg);
                break;
            }

            case TARG_XPATH:
                checkNsRefs(root, arg);
            case TARG_ATTR_STRING:
                xmlNewProp(newnode, newtarg->arguments[j].attrname, BAD_CAST arg);
                break;

            case TARG_STRING:
                xmlNodeAddContent(newnode, BAD_CAST arg);
                break;

            case TARG_NEWLINE:
                xmlNewProp(newnode, BAD_CAST "select", BAD_CAST "'\n'");
                break;
            case TARG_STR_NAME_SELECT:
                xmlNewProp(newnode, BAD_CAST "name", BAD_CAST "select");
                break;

            case TARG_INP_NAME:
                *use_inputfile = 1;
                xmlNewProp(newnode, BAD_CAST "select", BAD_CAST "$inputFile");
                break;

            case TARG_SORT_OP: {
                char order, data_type, case_order;
                int nread;
                nread = sscanf(arg, "%c:%c:%c", &order, &data_type, &case_order);
                if (nread != 3) {
                    /* TODO: allow missing letters */
                    fprintf(stderr, "sort requires (A|D):(N|T):(U|L), got %s\n", arg);
                    exit(EXIT_BAD_ARGS);
                }

                if (order == 'A' || order == 'D')
                    xmlNewProp(newnode, BAD_CAST "order",
                        BAD_CAST (order == 'A'? "ascending" : "descending"));
                if (data_type == 'N' || data_type == 'T')
                    xmlNewProp(newnode, BAD_CAST "data-type",
                        BAD_CAST (data_type == 'N'? "number" : "text"));
                if (case_order == 'U' || case_order == 'L')
                    xmlNewProp(newnode, BAD_CAST "case-order",
                        BAD_CAST (case_order == 'U'? "upper-first" : "lower-first"));
            } break;

            default:
                assert(0);
            }
        }

        switch (nesting) {
        case -1:
            do { node = node->parent; } while(node->_private);
            break;
        case 0:
            break;
        case 1:
            node = newnode;
            break;
        default:
            assert(0);
        }
        targ = newtarg;
    }

    if (templateEmpty)
    {
        fprintf(stderr, "error in arguments:");
        fprintf(stderr, " -t or --template option must be followed by");
        fprintf(stderr, " --match or other options\n");
        exit(EXIT_BAD_ARGS);
    }

    return have_next_template;
}

/**
 *  Prepare XSLT stylesheet based on command line options
 */
void
selPrepareXslt(xmlDocPtr style, selOptionsPtr ops)
{
    const xmlChar* TEMPLATE_NAME_FMT = BAD_CAST "t%d";
    xmlChar num_buf[1+10+1];    /* t+maxnumber+NUL */

    int ns, use_inputfile = 0, use_value_of = 0, templates = 0;
    xmlNodePtr root, root_template = NULL;
    xmlNsPtr xslns;
    xmlBufferPtr attr_buf;

    root = xmlNewDocRawNode(style, NULL, BAD_CAST "stylesheet", NULL);
    xmlDocSetRootElement(style, root);
    xmlNewProp(root, BAD_CAST "version", BAD_CAST "1.0");
    xslns = xmlNewNs(root, XSLT_NAMESPACE, BAD_CAST "xsl");
    xmlSetNs(root, xslns);

    ns = 0;
    while(ns_arr[ns])
    {
        xmlNewNs(root, ns_arr[ns+1], xmlStrlen(ns_arr[ns])?ns_arr[ns] : NULL);
        ns += 2;
    }
    cleanupNSArr(ns_arr);

    {
        xmlNodePtr output;
        output = xmlNewChild(root, xslns, BAD_CAST "output", NULL);
        xmlNewProp(output, BAD_CAST "omit-xml-declaration",
            BAD_CAST ((ops->no_omit_decl)?"no":"yes"));
        xmlNewProp(output, BAD_CAST "indent",
            BAD_CAST ((ops->indent)?"yes":"no"));
        if (ops->encoding) xmlNewProp(output, BAD_CAST "encoding", ops->encoding);
        if (ops->outText) xmlNewProp(output, BAD_CAST "method", BAD_CAST "text");
    }

    {
        const char* arg = get_arg(OPTION_NEXT);
        if (!arg || (strcmp(arg, "-t") != 0 && strcmp(arg, "--template") != 0)) {
            fprintf(stderr, "error in arguments:");
            fprintf(stderr, " no -t or --template options found\n");
            exit(EXIT_BAD_ARGS);
        }
    }

    for (;;) {
        xmlNodePtr template;
        int more_templates = 0;
        templates++;
        template = xmlNewChild(root, xslns, BAD_CAST "template", NULL);

        more_templates = selGenTemplate(root, template,
            xslns, ops, &use_inputfile, &use_value_of);
        if (!more_templates) break;
    }

    if (templates > 1) {
        int t;
        xmlNodePtr callee_template = root->last;
        root_template = xmlNewChild(root, xslns, BAD_CAST "template", NULL);

        for (t = 0; t < templates; t++) {
            xmlNodePtr call_template;

            call_template = xmlNewChild(root_template, xslns,
                BAD_CAST "call-template", NULL);

            xmlStrPrintf(num_buf, sizeof num_buf, TEMPLATE_NAME_FMT, t);
            xmlNewProp(call_template, BAD_CAST "name", num_buf);

            /* we are going over callee templates backwards  */
            xmlStrPrintf(num_buf, sizeof num_buf, TEMPLATE_NAME_FMT, templates-1 - t);
            xmlNewProp(callee_template, BAD_CAST "name", num_buf);

            callee_template = callee_template->prev;
        }
    } else {
        root_template = root->last;
    }

    if (!ops->outText && ops->printRoot) {
        xmlNodePtr result_root = root_template;
        xmlNodeSetName(result_root, BAD_CAST "xsl-select");
        xmlSetNs(result_root, NULL);
        xmlUnlinkNode(result_root);

        root_template = xmlNewChild(root, xslns, BAD_CAST "template", NULL);
        xmlAddChild(root_template, result_root);
    }

    xmlNewProp(root_template, BAD_CAST "match", BAD_CAST "/");

    attr_buf = xmlBufferCreate();
    for (ns = 0; ns < COUNT_OF(ns_entries); ns++) {
        if (xmlSearchNs(NULL, root, ns_entries[ns].prefix)) {
            if (xmlBufferLength(attr_buf) != 0)
                xmlBufferWriteChar(attr_buf, " ");
            xmlBufferCat(attr_buf, ns_entries[ns].prefix);
        }
    }
    if (xmlBufferLength(attr_buf) != 0)
        xmlNewProp(root, BAD_CAST "extension-element-prefixes",
            xmlBufferContent(attr_buf));

    xmlBufferFree(attr_buf);

    if (use_inputfile) {
        xmlNodePtr param;
        param = xmlNewChild(root, xslns, BAD_CAST "param", BAD_CAST "-");
        xmlNewProp(param, BAD_CAST "name", BAD_CAST "inputFile");
    }

    if (use_value_of) {
#       define XSLT_NS "http://www.w3.org/1999/XSL/Transform"

        xmlNodePtr value_of_template;
        int failed = xmlParseBalancedChunkMemory(style, NULL, NULL, 0, BAD_CAST
            "<xsl:template name='value-of-template' xmlns:xsl='"XSLT_NS"'>"
              "<xsl:param name='select'/>"
              "<xsl:value-of select='$select'/>"
              "<xsl:for-each select='exslt:node-set($select)[position()&gt;1]'>"
                "<xsl:value-of select='\"&#10;\"'/>"
                "<xsl:value-of select='.'/>"
              "</xsl:for-each>"
            "</xsl:template>",
            &value_of_template);
        if (failed) {
            /* should never happen */
            fprintf(stderr, "Error: value-of-template failed to parse!\n");
            exit(EXIT_LIB_ERROR);
        }
        xmlAddChild(root, value_of_template);

        /* get rid of redundant namespace declaration */
        xmlReconciliateNs(style, root);
        xmlFreeNs(value_of_template->nsDef);
        value_of_template->nsDef = NULL;

#       undef XSLT_NS
    }
}

/**
 * copy namespace definitions from @root to @style_tree
 */
static void
extract_ns_defs(xmlNodePtr root, xmlDocPtr style_tree)
{
    xmlNsPtr nsDef;
    xmlNodePtr style_root = xmlDocGetRootElement(style_tree);
    if (!root) return;

    for (nsDef = root->nsDef; nsDef; nsDef = nsDef->next) {
        xmlNewNs(style_root, nsDef->href, nsDef->prefix);
    }
}

static void
do_file(const char *filename, xmlDocPtr style_tree,
    int xml_options, const selOptions *ops, xsltOptions *xsltOps,
    int *status)
{
    xmlChar *value;
    xmlDocPtr doc;

    /* Pass input file name as predefined parameter 'inputFile' */
    const char *params[2+1] = { "inputFile" };
    value = xmlStrdup((const xmlChar *)"'");
    value = xmlStrcat(value, (const xmlChar *)filename);
    value = xmlStrcat(value, (const xmlChar *)"'");
    params[1] = (char *) value;


    doc = xmlReadFile(filename, NULL, xml_options);
    if (doc != NULL) {
        xmlDocPtr res;

        static xsltStylesheetPtr style = NULL;
        if (!style) {
            extract_ns_defs(xmlDocGetRootElement(doc), style_tree);
            /* Parse XSLT stylesheet */
            style = xsltParseStylesheetDoc(style_tree);
            if (!style) exit(EXIT_LIB_ERROR);
        }

        res = xsltTransform(xsltOps, doc, params, style, filename);
        if (!ops->quiet && (!res || xsltSaveResultToFile(stdout, res, style) < 0))
        {
            *status = EXIT_LIB_ERROR;
        }
        else if ((ops->quiet || *status == EXIT_FAILURE) && res && res->children)
        {
            *status = EXIT_SUCCESS;
            if (ops->quiet) exit(EXIT_SUCCESS);
        }
    } else {
        *status = EXIT_BAD_FILE;
    }

    xmlFree(value);
}

/**
 *  This is the main function for 'select' option
 */
int
selMain(void)
{
    static xsltOptions xsltOps;
    static selOptions ops;
    int status = EXIT_FAILURE;
    xmlDocPtr style_tree;
    int xml_options = 0;

    handle_namespace_args = 1;
    if (!get_arg(ARG_PEEK)) selUsage(EXIT_BAD_ARGS);

    selInitOptions(&ops);
    xsltInitOptions(&xsltOps);
    selParseOptions(&ops);
    xml_options |= XML_PARSE_NOENT; /* substitute entities */
    xml_options |= ops.nonet? XML_PARSE_NONET : 0;
    xsltOps.nonet = ops.nonet;
    xsltOps.noblanks = ops.noblanks;
    xsltInitLibXml(&xsltOps);
    xsltSetSortFunc(caseSortFunction);

    style_tree = xmlNewDoc(NULL);
    selPrepareXslt(style_tree, &ops);

    if (ops.printXSLT)
    {
        for (;;) {
            xmlTextReaderPtr reader;
            const char* filename = get_arg(ARG_NEXT);
            if (!filename) break;

            reader = xmlReaderForFile(filename, NULL, xml_options);
            xmlTextReaderRead(reader);
            extract_ns_defs(xmlTextReaderCurrentNode(reader), style_tree);
            xmlTextReaderClose(reader);
        }
        xmlDocFormatDump(stdout, style_tree, 1);
        exit(EXIT_SUCCESS);
    }

    if (get_arg(ARG_PEEK)) {
        for (;;) {
            const char* filename = get_arg(ARG_NEXT);
            if (!filename) break;
            do_file(filename, style_tree, xml_options, &ops, &xsltOps, &status);
        }
    } else {
        do_file("-", style_tree, xml_options, &ops, &xsltOps, &status);
    }

    /* 
     * Shutdown libxml
     */
    xsltCleanupGlobals();
    xmlCleanupParser();
    
    return status;
}




/****************************************************************************/

/**
 * @number: compare numerically?
 * @returns: negative if @obj1 compares less than @obj2
 */
static int
compareFunction(xmlXPathObjectPtr obj1, xmlXPathObjectPtr obj2,
    int number, int lower_first, int descending)
{
    int tst;

    if (number) {
        /* We make NaN smaller than number in accordance
           with XSLT spec */
        if (xmlXPathIsNaN(obj1->floatval)) {
            if (xmlXPathIsNaN(obj2->floatval))
                tst = 0;
            else
                tst = -1;
        } else if (xmlXPathIsNaN(obj2->floatval))
            tst = 1;
        else if (obj1->floatval == obj2->floatval)
            tst = 0;
        else if (obj1->floatval > obj2->floatval)
            tst = 1;
        else tst = -1;
    } else {
        tst = xmlStrcasecmp(obj1->stringval, obj2->stringval);
        if (tst == 0) {
            tst = xmlStrcmp(obj1->stringval, obj2->stringval);
            if (lower_first)
                tst = -tst;
        }
    }
    if (descending)
        tst = -tst;

    return tst;
}

/**
 * xsltSortFunction:
 * @ctxt:  a XSLT process context
 * @sorts:  array of sort nodes
 * @nbsorts:  the number of sorts in the array
 *
 * reorder the current node list accordingly to the set of sorting
 * requirement provided by the arry of nodes.
 *
 * like xsltDefaultSortFunction, but respect case-order attribute
 */
void
caseSortFunction(xsltTransformContextPtr ctxt, xmlNodePtr *sorts,
	           int nbsorts) {
#ifdef XSLT_REFACTORED
    xsltStyleItemSortPtr comp;
#else
    xsltStylePreCompPtr comp;
#endif
    xmlXPathObjectPtr *resultsTab[XSLT_MAX_SORT];
    xmlXPathObjectPtr *results = NULL, *res;
    xmlNodeSetPtr list = NULL;
    int descending, number, desc, numb;
    int len = 0;
    int i, j, incr;
    int tst;
    int depth;
    xmlNodePtr node;
    xmlXPathObjectPtr tmp;
    int tempstype[XSLT_MAX_SORT], temporder[XSLT_MAX_SORT],
        tempcaseorder[XSLT_MAX_SORT];

    if ((ctxt == NULL) || (sorts == NULL) || (nbsorts <= 0) ||
	(nbsorts >= XSLT_MAX_SORT))
	return;
    if (sorts[0] == NULL)
	return;
    comp = sorts[0]->psvi;
    if (comp == NULL)
	return;

    list = ctxt->nodeList;
    if ((list == NULL) || (list->nodeNr <= 1))
	return; /* nothing to do */

    for (j = 0; j < nbsorts; j++) {
	comp = sorts[j]->psvi;
	tempstype[j] = 0;
	if ((comp->stype == NULL) && (comp->has_stype != 0)) {
	    comp->stype =
		xsltEvalAttrValueTemplate(ctxt, sorts[j],
					  (const xmlChar *) "data-type",
					  XSLT_NAMESPACE);
	    if (comp->stype != NULL) {
		tempstype[j] = 1;
		if (xmlStrEqual(comp->stype, (const xmlChar *) "text"))
		    comp->number = 0;
		else if (xmlStrEqual(comp->stype, (const xmlChar *) "number"))
		    comp->number = 1;
		else {
		    xsltTransformError(ctxt, NULL, sorts[j],
			  "xsltDoSortFunction: no support for data-type = %s\n",
				     comp->stype);
		    comp->number = 0; /* use default */
		}
	    }
	}
	temporder[j] = 0;
	if ((comp->order == NULL) && (comp->has_order != 0)) {
	    comp->order = xsltEvalAttrValueTemplate(ctxt, sorts[j],
						    (const xmlChar *) "order",
						    XSLT_NAMESPACE);
	    if (comp->order != NULL) {
		temporder[j] = 1;
		if (xmlStrEqual(comp->order, (const xmlChar *) "ascending"))
		    comp->descending = 0;
		else if (xmlStrEqual(comp->order,
				     (const xmlChar *) "descending"))
		    comp->descending = 1;
		else {
		    xsltTransformError(ctxt, NULL, sorts[j],
			     "xsltDoSortFunction: invalid value %s for order\n",
				     comp->order);
		    comp->descending = 0; /* use default */
		}
	    }
	}

        tempcaseorder[j] = 0;
	if ((comp->case_order == NULL) /* && (comp->has_case_order != 0) */) {
	    comp->case_order = xsltEvalAttrValueTemplate(ctxt, sorts[j],
                (const xmlChar *) "case-order", XSLT_NAMESPACE);
	    if (comp->case_order != NULL) {
		tempcaseorder[j] = 1;
		if (xmlStrEqual(comp->case_order, BAD_CAST "upper-first"))
		    comp->lower_first = 0;
		else if (xmlStrEqual(comp->case_order, BAD_CAST "lower-first"))
		    comp->lower_first = 1;
		else {
		    xsltTransformError(ctxt, NULL, sorts[j],
                        "xsltDoSortFunction: invalid value %s for case-order\n",
                        comp->case_order);
		    comp->lower_first = 0; /* use default */
		}
	    }
	}
    }

    len = list->nodeNr;

    resultsTab[0] = xsltComputeSortResult(ctxt, sorts[0]);
    for (i = 1;i < XSLT_MAX_SORT;i++)
	resultsTab[i] = NULL;

    results = resultsTab[0];

    comp = sorts[0]->psvi;
    descending = comp->descending;
    number = comp->number;
    if (results == NULL)
	return;

    /* Shell's sort of node-set */
    for (incr = len / 2; incr > 0; incr /= 2) {
	for (i = incr; i < len; i++) {
	    j = i - incr;
	    if (results[i] == NULL)
		continue;

	    while (j >= 0) {
                if (results[j] == NULL)
                    tst = 1;
                else
                    tst = compareFunction(results[j], results[j + incr],
                        number, comp->lower_first, descending);

		if (tst == 0) {
		    /*
		     * Okay we need to use multi level sorts
		     */
		    depth = 1;
		    while (depth < nbsorts) {
			if (sorts[depth] == NULL)
			    break;
			comp = sorts[depth]->psvi;
			if (comp == NULL)
			    break;
			desc = comp->descending;
			numb = comp->number;

			/*
			 * Compute the result of the next level for the
			 * full set, this might be optimized ... or not
			 */
			if (resultsTab[depth] == NULL)
			    resultsTab[depth] = xsltComputeSortResult(ctxt,
				                        sorts[depth]);
			res = resultsTab[depth];
			if (res == NULL)
			    break;
			if (res[j] == NULL) {
			    if (res[j+incr] != NULL)
				tst = 1;
			} else {
                            tst = compareFunction(res[j], res[j+incr],
                                numb, comp->lower_first, desc);
                        }

			/*
			 * if we still can't differenciate at this level
			 * try one level deeper.
			 */
			if (tst != 0)
			    break;
			depth++;
		    }
		}
		if (tst == 0) {
		    tst = results[j]->index > results[j + incr]->index;
		}
		if (tst > 0) {
		    tmp = results[j];
		    results[j] = results[j + incr];
		    results[j + incr] = tmp;
		    node = list->nodeTab[j];
		    list->nodeTab[j] = list->nodeTab[j + incr];
		    list->nodeTab[j + incr] = node;
		    depth = 1;
		    while (depth < nbsorts) {
			if (sorts[depth] == NULL)
			    break;
			if (resultsTab[depth] == NULL)
			    break;
			res = resultsTab[depth];
			tmp = res[j];
			res[j] = res[j + incr];
			res[j + incr] = tmp;
			depth++;
		    }
		    j -= incr;
		} else
		    break;
	    }
	}
    }

    for (j = 0; j < nbsorts; j++) {
	comp = sorts[j]->psvi;
	if (tempstype[j] == 1) {
	    /* The data-type needs to be recomputed each time */
	    xmlFree((void *)(comp->stype));
	    comp->stype = NULL;
	}
	if (temporder[j] == 1) {
	    /* The order needs to be recomputed each time */
	    xmlFree((void *)(comp->order));
	    comp->order = NULL;
	}
	if (tempcaseorder[j] == 1) {
	    /* The case-order needs to be recomputed each time */
	    xmlFree((void *)(comp->case_order));
	    comp->case_order = NULL;
	}
	if (resultsTab[j] != NULL) {
	    for (i = 0;i < len;i++)
		xmlXPathFreeObject(resultsTab[j][i]);
	    xmlFree(resultsTab[j]);
	}
    }
}

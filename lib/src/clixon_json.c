/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2019 Olof Hagsand
  Copyright (C) 2020 Olof Hagsand and Rubicon Communications, LLC

  This file is part of CLIXON.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

  Alternatively, the contents of this file may be used under the terms of
  the GNU General Public License Version 3 or later (the "GPL"),
  in which case the provisions of the GPL are applicable instead
  of those above. If you wish to allow use of your version of this file only
  under the terms of the GPL, and not to allow others to
  use your version of this file under the terms of Apache License version 2, 
  indicate your decision by deleting the provisions above and replace them with
  the  notice and other provisions required by the GPL. If you do not delete
  the provisions above, a recipient may use your version of this file under
  the terms of any one of the Apache License version 2 or the GPL.

  ***** END LICENSE BLOCK *****

 * JSON support functions.
 * JSON syntax is according to:
 * http://www.ecma-international.org/publications/files/ECMA-ST/ECMA-404.pdf
 * RFC 7951 JSON Encoding of Data Modeled with YANG
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <syslog.h>

/* cligen */
#include <cligen/cligen.h>

/* clixon */
#include "clixon_err.h"
#include "clixon_log.h"
#include "clixon_queue.h"
#include "clixon_string.h"
#include "clixon_hash.h"
#include "clixon_handle.h"
#include "clixon_options.h"
#include "clixon_yang.h"
#include "clixon_xml.h"
#include "clixon_yang_type.h"
#include "clixon_yang_module.h"
#include "clixon_xml_sort.h"
#include "clixon_xml_map.h"
#include "clixon_xml_nsctx.h" /* namespace context */
#include "clixon_netconf_lib.h"
#include "clixon_json.h"
#include "clixon_json_parse.h"

#define JSON_INDENT 2 /* maybe we should set this programmatically? */

/* Let xml2json_cbuf_vec() return json array: [a,b].
   ALternative is to create a pseudo-object and return that: {top:{a,b}}
*/
#define VEC_ARRAY 1

/* Size of json read buffer when reading from file*/
#define BUFLEN 1024

/* Name of xml top object created by xml parse functions */
#define JSON_TOP_SYMBOL "top"

enum array_element_type{
    NO_ARRAY=0,
    FIRST_ARRAY,  /* [a, */
    MIDDLE_ARRAY, /*  a, */
    LAST_ARRAY,   /*  a] */
    SINGLE_ARRAY, /* [a] */
    BODY_ARRAY
};

enum childtype{
    NULL_CHILD=0, /* eg <a/> no children. Translated to null if in 
		   * array or leaf terminal, and to {} if proper object, ie container.
		   * anyxml/anydata?
		   */
    BODY_CHILD,   /* eg one child which is a body, eg <a>1</a> */
    ANY_CHILD,    /* eg <a><b/></a> or <a><b/><c/></a> */
};

/*! x is element and has exactly one child which in turn has none 
 * remove attributes from x
 * @see tleaf in clixon_xml_map.c
 */
static enum childtype
child_type(cxobj *x)
{
    cxobj *xc;   /* the only child of x */
    int    clen; /* nr of children */

    clen = xml_child_nr_notype(x, CX_ATTR);
    if (xml_type(x) != CX_ELMNT)
	return -1; /* n/a */
    if (clen == 0)
    	return NULL_CHILD;
    if (clen > 1)
	return ANY_CHILD;
    /* From here exactly one noattr child, get it */
    xc = NULL;
    while ((xc = xml_child_each(x, xc, -1)) != NULL)
	if (xml_type(xc) != CX_ATTR)
	    break;
    if (xc == NULL)
	return -2; /* n/a */
    if (xml_child_nr_notype(xc, CX_ATTR) == 0 && xml_type(xc)==CX_BODY)
	return BODY_CHILD;
    else
	return ANY_CHILD;
}

static char*
childtype2str(enum childtype lt)
{
    switch(lt){
    case NULL_CHILD:
	return "null";
	break;
    case BODY_CHILD:
	return "body";
	break;
    case ANY_CHILD:
	return "any";
	break;
    }
    return "";
}

static char*
arraytype2str(enum array_element_type lt)
{
    switch(lt){
    case NO_ARRAY:
	return "no";
	break;
    case FIRST_ARRAY:
	return "first";
	break;
    case MIDDLE_ARRAY:
	return "middle";
	break;
    case LAST_ARRAY:
	return "last";
	break;
    case SINGLE_ARRAY:
	return "single";
	break;
    case BODY_ARRAY:
	return "body";
	break;
    }
    return "";
}

/*! Check typeof x in array
 * Some complexity when x is in different namespaces
 */
static enum array_element_type
array_eval(cxobj *xprev, 
	   cxobj *x, 
	   cxobj *xnext)
{
    enum array_element_type array = NO_ARRAY;
    int                     eqprev=0;
    int                     eqnext=0;
    yang_stmt              *ys;
    char                   *nsx; /* namespace of x */
    char                   *ns2;

    nsx = xml_find_type_value(x, NULL, "xmlns", CX_ATTR);
    if (xml_type(x) != CX_ELMNT){
	array=BODY_ARRAY;
	goto done;
    }
    ys = xml_spec(x);
    if (xnext && 
	xml_type(xnext)==CX_ELMNT &&
	strcmp(xml_name(x), xml_name(xnext))==0){
        ns2 = xml_find_type_value(xnext, NULL, "xmlns", CX_ATTR);
	if ((!nsx && !ns2)
	    || (nsx && ns2 && strcmp(nsx,ns2)==0))
	    eqnext++;
    }
    if (xprev &&
	xml_type(xprev)==CX_ELMNT &&
	strcmp(xml_name(x),xml_name(xprev))==0){
	ns2 = xml_find_type_value(xprev, NULL, "xmlns", CX_ATTR);
	if ((!nsx && !ns2)
	    || (nsx && ns2 && strcmp(nsx,ns2)==0))
	    eqprev++;
    }
    if (eqprev && eqnext)
	array = MIDDLE_ARRAY;
    else if (eqprev)
	array = LAST_ARRAY;
    else if (eqnext)
	array = FIRST_ARRAY;
    else  if (ys && yang_keyword_get(ys) == Y_LIST)
	array = SINGLE_ARRAY;
    else
	array = NO_ARRAY;
 done:
    return array;
}

/*! Escape a json string as well as decode xml cdata
 * @param[out] cb   cbuf   (encoded)
 * @param[in]  str  string (unencoded)
 */
static int
json_str_escape_cdata(cbuf *cb,
		      char *str)
{
    int   retval = -1;
    int   i;
    int   esc = 0; /* cdata escape */

    for (i=0;i<strlen(str);i++)
	switch (str[i]){
	case '\n':
	    cprintf(cb, "\\n");
	    break;
	case '\"':
	    cprintf(cb, "\\\"");
	    break;
	case '\\':
	    cprintf(cb, "\\\\");
	    break;
	case '<':
	    if (!esc &&
		strncmp(&str[i], "<![CDATA[", strlen("<![CDATA[")) == 0){
		esc=1;
		i += strlen("<![CDATA[")-1;
	    }
	    else
		cprintf(cb, "%c", str[i]);
	    break;
	case ']':
	    if (esc &&
		strncmp(&str[i], "]]>", strlen("]]>")) == 0){
		esc=0;
		i += strlen("]]>")-1;
	    }
	    else
		cprintf(cb, "%c", str[i]);
	    break;
	default: /* fall thru */
	    cprintf(cb, "%c", str[i]);
	    break;
	}
    retval = 0;
    // done:
    return retval;
}

/*! Decode types from JSON to XML identityrefs
 * Assume an xml tree where prefix:name have been split into "module":"name"
 * In other words, from JSON RFC7951 to XML namespace trees
 * @param[in]     x     XML tree. Must be yang populated.
 * @param[in]     yspec Yang spec
 * @param[out]    xerr  Reason for invalid tree returned as netconf err msg or NULL
 * @retval        1     OK 
 * @retval        0     Invalid, wrt namespace.  xerr set
 * @retval       -1     Error
 * @see RFC7951 Sec 4 and 6.8
 */
static int
json2xml_decode_identityref(cxobj     *x,
			    yang_stmt *y,
			    cxobj    **xerr)
{
    int        retval = -1;
    char      *namespace;
    char      *body;
    cxobj     *xb;
    cxobj     *xa;
    char      *prefix = NULL;
    char      *id = NULL;
    yang_stmt *ymod;
    yang_stmt *yspec;
    cvec      *nsc = NULL;
    char      *prefix2 = NULL;
    cbuf      *cbv = NULL;
    
    clicon_debug(1, "%s", __FUNCTION__);
    yspec = ys_spec(y);
    if ((xb = xml_body_get(x)) == NULL)
	goto ok;
    body = xml_value(xb);
    if (nodeid_split(body, &prefix, &id) < 0)
	goto done;
    /* prefix is a module name -> find module */
    if (prefix){
	if ((ymod = yang_find_module_by_name(yspec, prefix)) != NULL){
	    namespace = yang_find_mynamespace(ymod);
	    /* Is this namespace in the xml context?
	     * (yes) use its prefix (unless it is NULL)
	     * (no)  insert a xmlns:<prefix> statement
	     * Get the whole namespace context from x
	     */
	    if (xml_nsctx_node(x, &nsc) < 0)
		goto done;
	    clicon_debug(1, "%s prefix:%s body:%s namespace:%s",
			 __FUNCTION__, prefix, body, namespace);
	    if (!xml_nsctx_get_prefix(nsc, namespace, &prefix2)){
		/* (no)  insert a xmlns:<prefix> statement
		 * Get yang prefix from import statement of my mod */
		if (yang_find_prefix_by_namespace(y, namespace, &prefix2) == 0){
#ifndef IDENTITYREF_KLUDGE
		    /* Just get the prefix from the module's own namespace */
		    if (xerr && netconf_unknown_namespace_xml(xerr, "application",
						      namespace,
						      "No local prefix corresponding to namespace") < 0)
			goto done;
		    goto fail;
#endif
		}
		/* if prefix2 is NULL here, we get the canonical prefix */
		if (prefix2 == NULL)
		    prefix2 = yang_find_myprefix(ymod);
		/* Add "xmlns:prefix2=namespace" */
		if ((xa = xml_new(prefix2, x, NULL)) == NULL)
		    goto done;
		xml_type_set(xa, CX_ATTR);
		if (xml_prefix_set(xa, "xmlns") < 0)
		    goto done;
		if (xml_value_set(xa, namespace) < 0)
		    goto done;
	    }
	    /* Here prefix2 is valid and can be NULL
	       Change body prefix to prefix2:id */
	    if ((cbv = cbuf_new()) == NULL){
		clicon_err(OE_XML, errno, "cbuf_new");
		goto done;
	    }
	    if (prefix2)
		cprintf(cbv, "%s:%s", prefix2, id);
	    else
		cprintf(cbv, "%s", id);

	    if (xml_value_set(xb, cbuf_get(cbv)) < 0)
		goto done;
	}
	else{
	    if (xerr && netconf_unknown_namespace_xml(xerr, "application",
					      prefix,
					      "No module corresponding to prefix") < 0)		
		goto done;
	    goto fail;
	}
    } /* prefix */
 ok:
    retval = 1;
 done:
    if (prefix)
	free(prefix);
    if (id)
	free(id);
    if (nsc)
	xml_nsctx_free(nsc);
    if (cbv)
	cbuf_free(cbv);
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Decode leaf/leaf_list types from JSON to XML after parsing and yang
 *
 * Assume an xml tree where prefix:name have been split into "module":"name"
 * In other words, from JSON RFC7951 to XML namespace trees
 * 
 * @param[in]     x     XML tree. Must be yang populated. After json parsing
 * @param[in]     yspec Yang spec
 * @param[out]    xerr  Reason for invalid tree returned as netconf err msg or NULL
 * @retval        1     OK 
 * @retval        0     Invalid, wrt namespace.  xerr set
 * @retval       -1     Error
 * @see RFC7951 Sec 4 and 6.8
 */
int
json2xml_decode(cxobj     *x,
		cxobj    **xerr)
{
    int           retval = -1;
    yang_stmt    *y;
    enum rfc_6020 keyword;
    cxobj        *xc;
    int           ret;
    yang_stmt    *ytype;

    if ((y = xml_spec(x)) != NULL){
	keyword = yang_keyword_get(y);
	if (keyword == Y_LEAF || keyword == Y_LEAF_LIST){
	    if (yang_type_get(y, NULL, &ytype, NULL, NULL, NULL, NULL, NULL) < 0)
		goto done;

	    if (ytype){
		if (strcmp(yang_argument_get(ytype), "identityref")==0){
		    if ((ret = json2xml_decode_identityref(x, y, xerr)) < 0)
			goto done;
		    if (ret == 0)
			goto fail;
		}
		else if (strcmp(yang_argument_get(ytype), "empty")==0)
		    ; /* dont need to do anything */
	    }
	}
    }
    xc = NULL;
    while ((xc = xml_child_each(x, xc, CX_ELMNT)) != NULL){
	if ((ret = json2xml_decode(xc, xerr)) < 0)
	    goto done;
	if (ret == 0)
	    goto fail;
    }
    retval = 1;
 done:
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Encode leaf/leaf_list identityref type from XML to JSON
 * @param[in]     x    XML body node
 * @param[in]     body body string
 * @param[in]     ys   Yang spec of parent
 * @param[out]    cb   Encoded string
 */
static int
xml2json_encode_identityref(cxobj     *xb,
			    char      *body,
			    yang_stmt *yp,
			    cbuf      *cb)
{
    int        retval = -1;
    char      *prefix = NULL;
    char      *id = NULL;
    char      *namespace = NULL;
    yang_stmt *ymod;
    yang_stmt *yspec;
    yang_stmt *my_ymod;

    clicon_debug(1, "%s %s", __FUNCTION__, body);
    my_ymod = ys_module(yp);
    yspec = ys_spec(yp);
    if (nodeid_split(body, &prefix, &id) < 0)
	goto done;
    /* prefix is xml local -> get namespace */
    if (xml2ns(xb, prefix, &namespace) < 0)
	goto done;
    /* We got the namespace, now get the module */
    //    clicon_debug(1, "%s body:%s prefix:%s namespace:%s", __FUNCTION__, body, prefix, namespace);
#ifdef IDENTITYREF_KLUDGE
    if (namespace == NULL){
    /* If we dont find namespace here, we assume it is because of a missing
     * xmlns that should be there, as a kludge we search for its (own)
     * prefix in mymodule.
    */
	if ((ymod = yang_find_module_by_prefix_yspec(yspec, prefix)) != NULL)
	    cprintf(cb, "%s:%s", yang_argument_get(ymod), id);
	else
	    cprintf(cb, "%s", id);
    }
    else
#endif
	{
    if ((ymod = yang_find_module_by_namespace(yspec, namespace)) != NULL){
	
	if (ymod == my_ymod)
	    cprintf(cb, "%s", id);
	else{
	    cprintf(cb, "%s:%s", yang_argument_get(ymod), id);
	}
    }
    else
	cprintf(cb, "%s", id);
	}
    retval = 0;
 done:
    if (prefix)
	free(prefix);
    if (id)
	free(id);
    return retval;
}

/*! Encode leaf/leaf_list types from XML to JSON
 * @param[in]     x   XML body
 * @param[in]     ys  Yang spec of parent
 * @param[out]    cb0  Encoded string
 */
static int
xml2json_encode(cxobj     *xb,
		cbuf      *cb0)
{
    int           retval = -1;
    cxobj        *xp;
    yang_stmt    *yp;
    enum rfc_6020 keyword;
    yang_stmt    *ytype;
    char         *restype;  /* resolved type */
    char         *origtype=NULL;   /* original type */
    char         *body;
    enum cv_type  cvtype;
    int           quote = 1; /* Quote value w string: "val" */
    cbuf         *cb = NULL; /* the variable itself */

    if ((cb = cbuf_new()) ==NULL){
	clicon_err(OE_XML, errno, "cbuf_new");
	goto done;
    }
    body = xml_value(xb);
    if ((xp = xml_parent(xb)) == NULL ||
	(yp = xml_spec(xp)) == NULL){
	cprintf(cb, "%s", body); 
	goto ok; /* unknown */
    }
    keyword = yang_keyword_get(yp);
    switch (keyword){
    case Y_LEAF:
    case Y_LEAF_LIST:
	if (yang_type_get(yp, &origtype, &ytype, NULL, NULL, NULL, NULL, NULL) < 0)
	    goto done;
	restype = ytype?yang_argument_get(ytype):NULL;
	cvtype = yang_type2cv(yp);
	switch (cvtype){ 
	case CGV_STRING:
	    if (ytype){
		if (strcmp(restype, "identityref")==0){
		    if (xml2json_encode_identityref(xb, body, yp, cb) < 0)
			goto done;
		}
		else
		    cprintf(cb, "%s", body);
	    }
	    else
		cprintf(cb, "%s", body);
	    break;
	case CGV_INT8:
	case CGV_INT16:
	case CGV_INT32:
	case CGV_INT64:
	case CGV_UINT8:
	case CGV_UINT16:
	case CGV_UINT32:
	case CGV_UINT64:
	case CGV_DEC64:
	case CGV_BOOL:
	    cprintf(cb, "%s", body);
	    quote = 0;
	    break;
	default:
	    cprintf(cb, "%s", body);
	}
	break;
    default:
	cprintf(cb, "%s", body);
	break;
    }
 ok:
    /* write into original cb0
     * includign quoting and encoding 
     */
    if (quote){
	cprintf(cb0, "\"");
	json_str_escape_cdata(cb0, cbuf_get(cb));
    }
    else
	cprintf(cb0, "%s", cbuf_get(cb));
    if (quote)
	cprintf(cb0, "\"");
    retval = 0;
 done:
    if (cb)
	cbuf_free(cb);
    if (origtype)
	free(origtype);
    return retval;
}

/*! Do the actual work of translating XML to JSON 
 * @param[out]   cb        Cligen text buffer containing json on exit
 * @param[in]    x         XML tree structure containing XML to translate
 * @param[in]    yp        Parent yang spec needed for body
 * @param[in]    arraytype Does x occur in a array (of its parent) and how?
 * @param[in]    level     Indentation level
 * @param[in]    pretty    Pretty-print output (2 means debug)
 * @param[in]    flat      Dont print NO_ARRAY object name (for _vec call)
 * @param[in]    bodystr   Set if value is string, 0 otherwise. Only if body
 *
 * @note Does not work with XML attributes
 * The following matrix explains how the mapping is done.
 * You need to understand what arraytype means (no/first/middle/last)
 * and what childtype is (null,body,any)
  +----------+--------------+--------------+--------------+
  |array,leaf| null         | body         | any          |
  +----------+--------------+--------------+--------------+
  |no        | <a/>         |<a>1</a>      |<a><b/></a>   |
  |          |              |              |              |
  |  json:   |\ta:null      |\ta:          |\ta:{\n       |
  |          |              |              |\n}           |
  +----------+--------------+--------------+--------------+
  |first     |<a/><a..      |<a>1</a><a..  |<a><b/></a><a.|
  |          |              |              |              |
  |  json:   |\ta:[\n\tnull |\ta:[\n\t     |\ta:[\n\t{\n  |
  |          |              |              |\n\t}         |
  +----------+--------------+--------------+--------------+
  |middle    |..a><a/><a..  |.a><a>1</a><a.|              |
  |          |              |              |              |
  |  json:   |\tnull        |\t            |\t{a          |
  |          |              |              |\n\t}         |
  +----------+--------------+--------------+--------------+
  |last      |..a></a>      |..a><a>1</a>  |              |
  |          |              |              |              |
  |  json:   |\tnull        |\t            |\t{a          |
  |          |\n\t]         |\n\t]         |\n\t}\t]      |
  +----------+--------------+--------------+--------------+
 */
static int 
xml2json1_cbuf(cbuf                   *cb,
	       cxobj                  *x,
	       enum array_element_type arraytype,
	       int                     level,
	       int                     pretty,
	       int                     flat,
	       char                   *modname0)
{
    int              retval = -1;
    int              i;
    cxobj           *xc;
    enum childtype   childt;
    enum array_element_type xc_arraytype;
    yang_stmt       *ys;
    yang_stmt       *ymod; /* yang module */
    int              commas;
    char            *modname = NULL;

    if ((ys = xml_spec(x)) != NULL){
	ymod = ys_real_module(ys);
	modname = yang_argument_get(ymod);
	if (modname0 && strcmp(modname, modname0) == 0)
	    modname=NULL;
	else
	    modname0 = modname; /* modname0 is ancestor ns passed to child */
    }
    childt = child_type(x);
    if (pretty==2)
	cprintf(cb, "#%s_array, %s_child ", 
		arraytype2str(arraytype),
		childtype2str(childt));
    switch(arraytype){
    case BODY_ARRAY: /* Only place in fn where body is printed */
	if (xml2json_encode(x, cb) < 0)
	    goto done;
	break;
    case NO_ARRAY:
	if (!flat){
	    cprintf(cb, "%*s\"", pretty?(level*JSON_INDENT):0, "");
	    if (modname) 
		cprintf(cb, "%s:", modname);
	    cprintf(cb, "%s\":%s", xml_name(x), pretty?" ":"");
	}
	switch (childt){
	case NULL_CHILD:
	    /* If x is a container, use {} instead of null 
	     * if leaf or leaf-list then assume EMPTY type, then [null]
	     * else null
	     */
	    if (ys && yang_keyword_get(ys) == Y_CONTAINER)
		cprintf(cb, "{}");
	    else{
		if (ys &&
		    (yang_keyword_get(ys) == Y_LEAF || yang_keyword_get(ys) == Y_LEAF_LIST))
		    cprintf(cb, "[null]");
		else
		    cprintf(cb, "null");
	    }
	    break;
	case BODY_CHILD:
	    break;
	case ANY_CHILD:
	    cprintf(cb, "{%s", pretty?"\n":"");
	    break;
	default:
	    break;
	}
	break;
    case FIRST_ARRAY:
    case SINGLE_ARRAY:
	cprintf(cb, "%*s\"", pretty?(level*JSON_INDENT):0, "");
	if (modname)
	    cprintf(cb, "%s:", modname);
	cprintf(cb, "%s\":%s", xml_name(x), pretty?" ":"");
	level++;
	cprintf(cb, "[%s%*s", 
		pretty?"\n":"",
		pretty?(level*JSON_INDENT):0, "");
	switch (childt){
	case NULL_CHILD:
	    cprintf(cb, "null");
	    break;
	case BODY_CHILD:
	    break;
	case ANY_CHILD:
	    cprintf(cb, "{%s", pretty?"\n":"");
	    break;
	default:
	    break;
	}
	break;
    case MIDDLE_ARRAY:
    case LAST_ARRAY:
	level++;
	cprintf(cb, "%*s", 
		pretty?(level*JSON_INDENT):0, "");
	switch (childt){
	case NULL_CHILD:
	    cprintf(cb, "null");
	    break;
	case BODY_CHILD:
	    break;
	case ANY_CHILD:
	    cprintf(cb, "{%s", pretty?"\n":"");
	    break;
	default:
	    break;
	}
	break;
    default:
	break;
    }
    /* Check for typed sub-body if:
     * arraytype=* but child-type is BODY_CHILD 
     * This is code for writing <a>42</a> as "a":42 and not "a":"42"
     */
    commas = xml_child_nr_notype(x, CX_ATTR) - 1;
    for (i=0; i<xml_child_nr(x); i++){
	xc = xml_child_i(x, i);
	if (xml_type(xc) == CX_ATTR)
	    continue; /* XXX Only xmlns attributes mapped */

	xc_arraytype = array_eval(i?xml_child_i(x,i-1):NULL, 
				xc, 
				xml_child_i(x, i+1));
	if (xml2json1_cbuf(cb, 
			   xc, 
			   xc_arraytype,
			   level+1, pretty, 0, modname0) < 0)
	    goto done;
	if (commas > 0) {
	    cprintf(cb, ",%s", pretty?"\n":"");
	    --commas;
	}
    }
    switch (arraytype){
    case BODY_ARRAY:
	break;
    case NO_ARRAY:
	switch (childt){
	case NULL_CHILD:
	case BODY_CHILD:
	    break;
	case ANY_CHILD:
	    cprintf(cb, "%s%*s}", 
		    pretty?"\n":"",
		    pretty?(level*JSON_INDENT):0, "");
	    break;
	default:
	    break;
	}
	level--;
	break;
    case FIRST_ARRAY:
    case MIDDLE_ARRAY:
	switch (childt){
	case NULL_CHILD:
	case BODY_CHILD:
	    break;
	case ANY_CHILD:
	    cprintf(cb, "%s%*s}", 
		    pretty?"\n":"",
		    pretty?(level*JSON_INDENT):0, "");
	    level--;
	    break;
	default:
	    break;
	}
	break;
    case SINGLE_ARRAY:
    case LAST_ARRAY:
	switch (childt){
	case NULL_CHILD:
	case BODY_CHILD:
	    cprintf(cb, "%s",pretty?"\n":"");
	    break;
	case ANY_CHILD:
	    cprintf(cb, "%s%*s}", 
		    pretty?"\n":"",
		    pretty?(level*JSON_INDENT):0, "");
	    cprintf(cb, "%s",pretty?"\n":"");
	    level--;
	    break;
	default:
	    break;
	}
	cprintf(cb, "%*s]",
		pretty?(level*JSON_INDENT):0,"");
	break;
    default:
	break;
    }
    retval = 0;
 done:
    return retval;
}

/*! Translate an XML tree to JSON in a CLIgen buffer
 *
 * XML-style namespace notation in tree, but RFC7951 in output assume yang 
 * populated 
 *
 * @param[in,out] cb     Cligen buffer to write to
 * @param[in]     x      XML tree to translate from
 * @param[in]     pretty Set if output is pretty-printed
 * @param[in]     top    By default only children are printed, set if include top
 * @retval        0      OK
 * @retval       -1      Error
 *
 * @code
 * cbuf *cb;
 * cb = cbuf_new();
 * if (xml2json_cbuf(cb, xn, 0, 1) < 0)
 *   goto err;
 * cbuf_free(cb);
 * @endcode
 * @see clicon_xml2cbuf
 */
int 
xml2json_cbuf(cbuf      *cb, 
	      cxobj     *x, 
	      int        pretty)
{
    int    retval = 1;
    int    level = 0;

    cprintf(cb, "%*s{%s", 
	    pretty?level*JSON_INDENT:0,"", 
	    pretty?"\n":"");
    if (xml2json1_cbuf(cb, 
		       x, 
		       NO_ARRAY,
		       level+1,
		       pretty,
		       0,
		       NULL /* ancestor modname / namespace */
		       ) < 0)
	goto done;
    cprintf(cb, "%s%*s}%s", 
	    pretty?"\n":"",
	    pretty?level*JSON_INDENT:0,"",
	    pretty?"\n":"");

    retval = 0;
 done:
    return retval;
}

/*! Translate a vector of xml objects to JSON Cligen buffer.
 * This is done by adding a top pseudo-object, and add the vector as subs,
 * and then not printing the top pseudo-object using the 'flat' option.
 * @param[out] cb     Cligen buffer to write to
 * @param[in]  vec    Vector of xml objecst
 * @param[in]  veclen Length of vector
 * @param[in]  pretty Set if output is pretty-printed (2 for debug)
 * @retval     0      OK
 * @retval    -1      Error
 * @note This only works if the vector is uniform, ie same object name.
 * Example: <b/><c/> --> <a><b/><c/></a> --> {"b" : null,"c" : null}
 * @see xml2json1_cbuf
 */
int 
xml2json_cbuf_vec(cbuf      *cb, 
		  cxobj    **vec,
		  size_t     veclen,
		  int        pretty)
{
    int    retval = -1;
    int    level = 0;
    cxobj *xp = NULL;
    int    i;
    cxobj *xc;
    cvec  *nsc = NULL; 

    if ((xp = xml_new("xml2json", NULL, NULL)) == NULL)
	goto done;
    /* Make a copy of old and graft it into new top-object
     * Also copy namespace context */
    for (i=0; i<veclen; i++){
	if (xml_nsctx_node(vec[i], &nsc) < 0)
	    goto done;
	if ((xc = xml_dup(vec[i])) == NULL)
	    goto done;
	xml_addsub(xp, xc);
	nscache_replace(xc, nsc); 
	nsc = NULL; /* nsc consumed */
    }
    if (0){
	cprintf(cb, "[%s", pretty?"\n":" ");
	level++;
    }
    if (xml2json1_cbuf(cb, 
		       xp, 
		       NO_ARRAY,
		       level+1, pretty,
		       1, NULL) < 0)
	goto done;

    if (0){
	level--;
	cprintf(cb, "%s]%s", 
	    pretty?"\n":"",
	    pretty?"\n":""); /* top object */
    }
    retval = 0;
 done:
    if (nsc)
	xml_nsctx_free(nsc);
    if (xp)
	xml_free(xp);
    return retval;
}

/*! Translate from xml tree to JSON and print to file
 * @param[in]  f      File to print to
 * @param[in]  x      XML tree to translate from
 * @param[in]  pretty Set if output is pretty-printed
 * @retval     0      OK
 * @retval    -1      Error
 *
 * @note yang is necessary to translate to one-member lists,
 * eg if a is a yang LIST <a>0</a> -> {"a":["0"]} and not {"a":"0"}
 * @code
 * if (xml2json(stderr, xn, 0) < 0)
 *   goto err;
 * @endcode
 */
int 
xml2json(FILE      *f, 
	 cxobj     *x, 
	 int        pretty)
{
    int   retval = 1;
    cbuf *cb = NULL;

    if ((cb = cbuf_new()) ==NULL){
	clicon_err(OE_XML, errno, "cbuf_new");
	goto done;
    }
    if (xml2json_cbuf(cb, x, pretty) < 0)
	goto done;
    fprintf(f, "%s", cbuf_get(cb));
    retval = 0;
 done:
    if (cb)
	cbuf_free(cb);
    return retval;
}

/*! Print an XML tree structure to an output stream as JSON
 *
 * @param[in]   f           UNIX output stream
 * @param[in]   xn          clicon xml tree
 */
int
json_print(FILE  *f, 
	   cxobj *xn)
{
    return xml2json(f, xn, 1);
}

/*! Translate a vector of xml objects to JSON File.
 * This is done by adding a top pseudo-object, and add the vector as subs,
 * and then not pritning the top pseudo-.object using the 'flat' option.
 * @param[out] cb     Cligen buffer to write to
 * @param[in]  vec    Vector of xml objecst
 * @param[in]  veclen Length of vector
 * @param[in]  pretty Set if output is pretty-printed (2 for debug)
 * @retval     0      OK
 * @retval    -1      Error
 * @note This only works if the vector is uniform, ie same object name.
 * Example: <b/><c/> --> <a><b/><c/></a> --> {"b" : null,"c" : null}
 * @see xml2json1_cbuf
 */
int 
xml2json_vec(FILE      *f, 
	     cxobj    **vec,
	     size_t     veclen,
	     int        pretty)
{
    int   retval = 1;
    cbuf *cb = NULL;

    if ((cb = cbuf_new()) == NULL){
	clicon_err(OE_XML, errno, "cbuf_new");
	goto done;
    }
    if (xml2json_cbuf_vec(cb, vec, veclen, pretty) < 0)
	goto done;
    fprintf(f, "%s", cbuf_get(cb));
    retval = 0;
 done:
    if (cb)
	cbuf_free(cb);
    return retval;
}

/*! Translate from JSON module:name to XML default ns: xmlns="uri" recursively
 * Assume an xml tree where prefix:name have been split into "module":"name"
 * In other words, from JSON to XML namespace trees
 * 
 * @param[in]     yspec Yang spec
 * @param[in,out] x     XML tree. Translate it in-line
 * @param[out]    xerr  Reason for invalid tree returned as netconf err msg or NULL
 * @retval        1     OK 
 * @retval        0     Invalid, wrt namespace.  xerr set
 * @retval       -1     Error
 * @note the opposite - xml2ns is made inline in xml2json1_cbuf
 * Example: <top><module:input> --> <top><input xmlns="">
 * @see RFC7951 Sec 4
 */
static int
json_xmlns_translate(yang_stmt *yspec,
		     cxobj     *x,
		     cxobj    **xerr)
{
    int        retval = -1;
    yang_stmt *ymod;
    char      *namespace;
    char      *modname = NULL;
    cxobj     *xc;
    int        ret;
    
    if ((modname = xml_prefix(x)) != NULL){ /* prefix is here module name */
	if ((ymod = yang_find_module_by_name(yspec, modname)) == NULL){
	    if (xerr &&
		netconf_unknown_namespace_xml(xerr, "application",
					      modname,
					      "No yang module found corresponding to prefix") < 0)
		goto done;
	    goto fail;
	}
	namespace = yang_find_mynamespace(ymod);
	/* It would be possible to use canonical prefixes here, but probably not
	 * necessary or even right. Therefore, the namespace given by the JSON prefix / module
	 * is always the default namespace with prefix NULL.
	 * If not, this would be the prefix to pass instead of NULL
	 * prefix = yang_find_myprefix(ymod);
	 */
	if (xml_namespace_change(x, namespace, NULL) < 0)
	    goto done;
    }
    xc = NULL;
    while ((xc = xml_child_each(x, xc, CX_ELMNT)) != NULL){
	if ((ret = json_xmlns_translate(yspec, xc, xerr)) < 0)
	    goto done;
	if (ret == 0)
	    goto fail;
    }
    retval = 1;
 done:
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Parse a string containing JSON and return an XML tree
 *
 * Parsing using yacc according to JSON syntax. Names with <prefix>:<id>
 * are split and interpreted as in RFC7951
 *
 * @param[in]  str    Input string containing JSON
 * @param[in]  yb     How to bind yang to XML top-level when parsing
 * @param[in]  yspec  If set, also do yang validation
 * @param[out] xt     XML top of tree typically w/o children on entry (but created)
 * @param[out] xerr   Reason for invalid returned as netconf err msg 
 * 
 * @see _xml_parse for XML variant
 * @retval        1   OK and valid
 * @retval        0   Invalid (only if yang spec)
 * @retval       -1   Error with clicon_err called
 * @see http://www.ecma-international.org/publications/files/ECMA-ST/ECMA-404.pdf
 * @see RFC 7951
 */
static int 
_json_parse(char          *str, 
	    enum yang_bind yb,
	    yang_stmt     *yspec,
	    cxobj         *xt,
	    cxobj        **xerr)
{
    int              retval = -1;
    clixon_json_yacc jy = {0,};
    int              ret;
    cxobj           *x;
    cbuf            *cberr = NULL;
    int              i;
    int              failed = 0; /* yang assignment */
    
    clicon_debug(1, "%s %d %s", __FUNCTION__, yb, str);
    jy.jy_parse_string = str;
    jy.jy_linenum = 1;
    jy.jy_current = xt;
    jy.jy_xtop = xt;
    if (json_scan_init(&jy) < 0)
	goto done;
    if (json_parse_init(&jy) < 0)
	goto done;
    if (clixon_json_parseparse(&jy) != 0) { /* yacc returns 1 on error */
	clicon_log(LOG_NOTICE, "JSON error: line %d", jy.jy_linenum);
	if (clicon_errno == 0)
	    clicon_err(OE_XML, 0, "JSON parser error with no error code (should not happen)");
	goto done;
    }
    /* Traverse new objects */
    for (i = 0; i < jy.jy_xlen; i++) {
	x = jy.jy_xvec[i];
	/* RFC 7951 Section 4: A namespace-qualified member name MUST be used for all 
	 * members of a top-level JSON object 
	 */
	if (yspec && xml_prefix(x) == NULL){
	    if ((cberr = cbuf_new()) == NULL){
		clicon_err(OE_UNIX, errno, "cbuf_new");
		goto done;
	    }
	    cprintf(cberr, "Top-level JSON object %s is not qualified with namespace which is a MUST according to RFC 7951", xml_name(x));
	    if (netconf_malformed_message_xml(xerr, cbuf_get(cberr)) < 0)
		goto done;
	    goto fail;
	}
	/* Names are split into name/prefix, but now add namespace info */
	if ((ret = json_xmlns_translate(yspec, x, xerr)) < 0)
	    goto done;
	if (ret == 0)
	    goto fail;
	/* Now assign yang stmts to each XML node 
	 * XXX should be xml_spec_populate0_parent() sometimes.
	 */
	switch (yb){
	case YB_RPC:
	case YB_UNKNOWN:
	case YB_NONE:
	    break;
	case YB_PARENT:
	    if ((ret = xml_spec_populate0_parent(x, xerr)) < 0)
		    goto done;
	    if (ret == 0)
		failed++;
	    break;
	case YB_TOP:
	    if (xml_spec_populate0(x, yspec, xerr) < 0)
		goto done;
	    if (ret == 0)
		failed++;
	    break;
	}
	/* Now find leafs with identityrefs (+transitive) and translate 
	 * prefixes in values to XML namespaces */
	if ((ret = json2xml_decode(x, xerr)) < 0)
	    goto done;
	if (ret == 0) /* XXX necessary? */
	    goto fail;
    }
    if (xml_apply0(xt, CX_ELMNT, xml_sort, NULL) < 0)
	goto done;
    retval = (failed==0) ? 1 : 0;
 done:
    clicon_debug(1, "%s retval:%d", __FUNCTION__, retval);
    if (cberr)
	cbuf_free(cberr);
    json_parse_exit(&jy);
    json_scan_exit(&jy);
    if (jy.jy_xvec)
	free(jy.jy_xvec);
    return retval; 
 fail: /* invalid */
    retval = 0;
    goto done;
}

/*! Parse string containing JSON and return an XML tree
 *
 * @param[in]     str   String containing JSON
 * @param[in]     yspec Yang specification, mandatory to make module->xmlns translation
 * @param[in,out] xt    Top object, if not exists, on success it is created with name 'top'
 * @param[out]    xerr  Reason for invalid returned as netconf err msg 
 *
 * @code
 *  cxobj *cx = NULL;
 *  if (json_parse_str(str, yspec, &cx, &xerr) < 0)
 *    err;
 *  xml_free(cx);
 * @endcode
 * @note  you need to free the xml parse tree after use, using xml_free()
 * @see json_parse_file
 * @retval        1     OK and valid
 * @retval        0     Invalid (only if yang spec) w xerr set
 * @retval       -1     Error with clicon_err called
 * @see json_parse_file with a file descriptor (and more description)
 */
int 
json_parse_str2(char          *str, 
		enum yang_bind yb,
		yang_stmt     *yspec,
		cxobj        **xt,
		cxobj        **xerr)
{
    clicon_debug(1, "%s", __FUNCTION__);
    if (xt==NULL){
	clicon_err(OE_XML, EINVAL, "xt is NULL");
	return -1;
    }
    if (*xt == NULL){
	if ((*xt = xml_new("top", NULL, NULL)) == NULL)
	    return -1;
    }
    return _json_parse(str, yb, yspec, *xt, xerr);
}

int 
json_parse_str(char      *str, 
	       yang_stmt *yspec,
	       cxobj    **xt,
	       cxobj    **xerr)
{
    enum yang_bind yb = YB_PARENT;
    
    clicon_debug(1, "%s", __FUNCTION__);
    if (xt==NULL){
	clicon_err(OE_XML, EINVAL, "xt is NULL");
	return -1;
    }
    if (*xt == NULL){
	yb = YB_TOP; /* ad-hoc #1 */
	if ((*xt = xml_new("top", NULL, NULL)) == NULL)
	    return -1;
    }
    else{
	if (xml_spec(*xt) == NULL)
	    yb = YB_TOP;  /* ad-hoc #2 */
    }
    return _json_parse(str, yb, yspec, *xt, xerr);
}

/*! Read a JSON definition from file and parse it into a parse-tree. 
 *
 * File will be parsed as follows:
 *   (1) parsed according to JSON; # Only this check if yspec is NULL
 *   (2) sanity checked wrt yang  
 *   (3) namespaces check (using <ns>:<name> notation
 *   (4) an xml parse tree will be returned
 * Note, only (1) and (4) will be done if yspec is NULL.
 * Part of (3) is to split json names if they contain colon, 
 *   eg: name="a:b" -> prefix="a", name="b"
 * But this is not done if yspec=NULL, and is not part of the JSON spec
 * 
 * @param[in]     fd    File descriptor to the JSON file (ASCII string)
 * @param[in]     yspec Yang specification, or NULL
 * @param[in,out] xt    Pointer to (XML) parse tree. If empty, create.
 * @param[out]    xerr  Reason for invalid returned as netconf err msg 
 *
 * @code
 *  cxobj *xt = NULL;
 *  if (json_parse_file(0, yspec, &xt) < 0)
 *    err;
 *  xml_free(xt);
 * @endcode
 * @note  you need to free the xml parse tree after use, using xml_free()
 * @note, If xt empty, a top-level symbol will be added so that <tree../> will be:  <top><tree.../></tree></top>
 * @note May block on file I/O
 *
 * @retval        1     OK and valid
 * @retval        0     Invalid (only if yang spec) w xerr set
 * @retval       -1     Error with clicon_err called
 *
 * @see json_parse_str
 * @see RFC7951
 */
int
json_parse_file(int        fd,
		yang_stmt *yspec,
		cxobj    **xt,
		cxobj    **xerr)
{
    int   retval = -1;
    int   ret;
    char *jsonbuf = NULL;
    int   jsonbuflen = BUFLEN; /* start size */
    int   oldjsonbuflen;
    char *ptr;
    char  ch;
    int   len = 0;
    enum yang_bind yb = YB_PARENT;
    
    if (xt==NULL){
	clicon_err(OE_XML, EINVAL, "xt is NULL");
	return -1;
    }
    if (*xt==NULL)
	yb = YB_TOP;
    if ((jsonbuf = malloc(jsonbuflen)) == NULL){
	clicon_err(OE_XML, errno, "malloc");
	goto done;
    }
    memset(jsonbuf, 0, jsonbuflen);
    ptr = jsonbuf;
    while (1){
	if ((ret = read(fd, &ch, 1)) < 0){
	    clicon_err(OE_XML, errno, "read");
	    break;
	}
	if (ret != 0)
	    jsonbuf[len++] = ch;
	if (ret == 0){
	    if (*xt == NULL)
		if ((*xt = xml_new(JSON_TOP_SYMBOL, NULL, NULL)) == NULL)
		    goto done;
	    if (len){
		if ((ret = _json_parse(ptr, yb, yspec, *xt, xerr)) < 0)
		    goto done;
		if (ret == 0)
		    goto fail;
	    }
	    break;
	}
	if (len>=jsonbuflen-1){ /* Space: one for the null character */
	    oldjsonbuflen = jsonbuflen;
	    jsonbuflen *= 2;
	    if ((jsonbuf = realloc(jsonbuf, jsonbuflen)) == NULL){
		clicon_err(OE_XML, errno, "realloc");
		goto done;
	    }
	    memset(jsonbuf+oldjsonbuflen, 0, jsonbuflen-oldjsonbuflen);
	    ptr = jsonbuf;
	}
    }
    retval = 1;
 done:
    if (retval < 0 && *xt){
	free(*xt);
	*xt = NULL;
    }
    if (jsonbuf)
	free(jsonbuf);
    return retval;    
 fail:
    retval = 0;
    goto done;
}



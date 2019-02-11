/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2019 Olof Hagsand and Benny Holmgren

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

 * XML search functions when used with YANG
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <limits.h>
#include <fnmatch.h>
#include <stdint.h>
#include <assert.h>
#include <syslog.h>

/* cligen */
#include <cligen/cligen.h>

/* clixon */
#include "clixon_err.h"
#include "clixon_log.h"
#include "clixon_string.h"
#include "clixon_queue.h"
#include "clixon_hash.h"
#include "clixon_handle.h"
#include "clixon_yang.h"
#include "clixon_yang_type.h"
#include "clixon_xml.h"
#include "clixon_options.h"
#include "clixon_xml_map.h"
#include "clixon_xml_sort.h"

/*! Get xml body value as cligen variable
 * @param[in]  x   XML node (body and leaf/leaf-list)
 * @param[out] cvp Pointer to cligen variable containing value of x body
 * @retval     0   OK, cvp contains cv or NULL
 * @retval    -1   Error
 * @note only applicable if x is body and has yang-spec and is leaf or leaf-list
 * Move to clixon_xml.c?
 */
static int
xml_cv_cache(cxobj   *x,
	     char    *body,
	     cg_var **cvp)
{
    int          retval = -1;
    cg_var      *cv = NULL;
    yang_stmt   *y;
    yang_stmt   *yrestype;
    enum cv_type cvtype;
    int          ret;
    char        *reason=NULL;
    int          options = 0;
    uint8_t      fraction = 0;
	    
    if ((cv = xml_cv(x)) != NULL)
	goto ok;
    if ((y = xml_spec(x)) == NULL)
	goto done;
    if (yang_type_get(y, NULL, &yrestype, &options, NULL, NULL, &fraction) < 0)
	goto done;
    yang2cv_type(yrestype->ys_argument, &cvtype);
    if (cvtype==CGV_ERR){
	clicon_err(OE_YANG, errno, "yang->cligen type %s mapping failed",
		   yrestype->ys_argument);
	goto done;
    }
    if ((cv = cv_new(cvtype)) == NULL){
	clicon_err(OE_YANG, errno, "cv_new");
	goto done;
    }
    if (cvtype == CGV_DEC64)
	cv_dec64_n_set(cv, fraction);
	
    if ((ret = cv_parse1(body, cv, &reason)) < 0){
	clicon_err(OE_YANG, errno, "cv_parse1");
	goto done;
    }
    if (ret == 0){
	clicon_err(OE_YANG, EINVAL, "cv parse error: %s\n", reason);
	goto done;
    }
    if (xml_cv_set(x, cv) < 0)
	goto done;
 ok:
    *cvp = cv;
    cv = NULL;
    retval = 0;
 done:
    if (reason)
	free(reason);
    if (cv)
	cv_free(cv);
    return retval;
}

/*! Given a child name and an XML object, return yang stmt of child
 * If no xml parent, find root yang stmt matching name
 * @param[in]  x        Child
 * @param[in]  xp       XML parent, can be NULL.
 * @param[in]  yspec    Yang specification (top level)
 * @param[out] yresult  Pointer to yang stmt of result, or NULL, if not found
 * @retval     0       OK
 * @retval    -1       Error
 * @note special rule for rpc, ie <rpc><foo>,look for top "foo" node.
 * @note works for import prefix, but not work for generic XML parsing where
 *       xmlns and xmlns:ns are used.
 */
int
xml_child_spec(cxobj      *x,
	       cxobj      *xp,
	       yang_spec  *yspec,
	       yang_stmt **yresult)
{
    int        retval = -1;
    yang_stmt *y = NULL;  /* result yang node */   
    yang_stmt *yparent; /* parent yang */
    yang_stmt *ymod = NULL;
    yang_stmt *yi;
    char      *name;
	    
    name = xml_name(x);
    if (xp && (yparent = xml_spec(xp)) != NULL){
	if (yparent->ys_keyword == Y_RPC){
	    if ((yi = yang_find((yang_node*)yparent, Y_INPUT, NULL)) != NULL)
		y = yang_find_datanode((yang_node*)yi, name);
	}
	else
	    y = yang_find_datanode((yang_node*)yparent, name);
    }
    else if (yspec){
	if (ys_module_by_xml(yspec, xp, &ymod) < 0)
	    goto done;
	if (ymod != NULL)
	    y = yang_find_schemanode((yang_node*)ymod, name);
	if (y == NULL && !_CLICON_XML_NS_STRICT){
	    if (xml_yang_find_non_strict(x, yspec, &y) < 0) /* schemanode */
		goto done;
	}
    }
    else
	y = NULL;
    /* kludge rpc -> input */
    if (y && y->ys_keyword == Y_RPC && yang_find((yang_node*)y, Y_INPUT, NULL))
	y = yang_find((yang_node*)y, Y_INPUT, NULL);
    *yresult = y;
    retval = 0;
 done:
    return retval;
}

/*! Help function to qsort for sorting entries in xml child vector
 * @param[in]  arg1 - actually cxobj**
 * @param[in]  arg2 - actually cxobj**
 * @retval  0  If equal
 * @retval <0  if arg1 is less than arg2
 * @retval >0  if arg1 is greater than arg2
 * @note args are pointer ot pointers, to fit into qsort cmp function
 * @see xml_cmp1   Similar, but for one object
 * @note empty value/NULL is smallest value
 */
static int
xml_cmp(const void* arg1, 
	const void* arg2)
{
    cxobj *x1 = *(struct xml**)arg1;
    cxobj *x2 = *(struct xml**)arg2;
    yang_stmt  *y1;
    yang_stmt  *y2;
    int         yi1;
    int         yi2;
    cvec       *cvk = NULL; /* vector of index keys */
    cg_var     *cvi;
    int         equal = 0;
    char       *b1;
    char       *b2;
    char       *keyname;
    cg_var     *cv1; 
    cg_var     *cv2;

    assert(x1&&x2);
    y1 = xml_spec(x1);
    y2 = xml_spec(x2);
    if (y1==NULL || y2==NULL)
	return 0; /* just ignore */
    if (y1 != y2){
	yi1 = yang_order(y1);
	yi2 = yang_order(y2);
	if ((equal = yi1-yi2) != 0)
	    return equal;
    }
    /* Now y1==y2, same Yang spec, can only be list or leaf-list,
     * But first check exceptions, eg config false or ordered-by user
     * otherwise sort according to key
     */
    if (yang_config(y1)==0 ||
	yang_find((yang_node*)y1, Y_ORDERED_BY, "user") != NULL)
	return 0; /* Ordered by user or state data : maintain existing order */
    switch (y1->ys_keyword){
    case Y_LEAF_LIST: /* Match with name and value */
	if ((b1 = xml_body(x1)) == NULL)
	    equal = -1;
	else if ((b2 = xml_body(x2)) == NULL)
	    equal = 1;
	else{
	    if (xml_cv_cache(x1, b1, &cv1) < 0)
		goto done;
	    if (xml_cv_cache(x2, b2, &cv2) < 0)
		goto done;
	    equal = cv_cmp(cv1, cv2);
	}
	break;
    case Y_LIST: /* Match with key values 
		  * Use Y_LIST cache (see struct yang_stmt)
		  */
	cvk = y1->ys_cvec; /* Use Y_LIST cache, see ys_populate_list() */
	cvi = NULL;
	while ((cvi = cvec_each(cvk, cvi)) != NULL) {
	    keyname = cv_string_get(cvi);
	    b1 = xml_find_body(x1, keyname);
	    b2 = xml_find_body(x2, keyname);
	    if ((equal = strcmp(b1,b2)) != 0)
		goto done;
	}
	equal = 0;
	break;
    default:
	break;
    }
 done:
    return equal;
}

/*! Compare xml object
 * @param[in]  x        XML node to compare with
 * @param[in]  y        The yang spec of x
 * @param[in]  name     Name to compare with x
 * @param[in]  keyword  Yang keyword (stmt type) to compare w x/y
 * @param[in]  keynr    Length of keyvec/keyval vector when applicable
 * @param[in]  keyvec   Array of of yang key identifiers
 * @param[in]  keyval   Array of of yang key values
 * @param[out] userorder If set, this yang order is user ordered, linear search
 * @retval  0  If equal (or userorder set)
 * @retval <0  if arg1 is less than arg2
 * @retval >0  if arg1 is greater than arg2
 * @see xml_cmp   Similar, but for two objects
 * @note Does not care about y type of value as xml_cmp
 */
static int
xml_cmp1(cxobj        *x,
	 yang_stmt    *y,
	 char         *name,
	 enum rfc_6020 keyword,   
	 int           keynr,
	 char        **keyvec,
	 char        **keyval,
	 int          *userorder)
{
    char      *b;
    int        i;
    char      *keyname;
    char      *key;
    int        match = 0;

    /* state data = userorder */
    if (userorder && yang_config(y)==0)
	*userorder=1;
    /* Check if same yang spec (order in yang stmt list) */
    switch (keyword){
    case Y_CONTAINER: /* Match with name */
    case Y_LEAF: /* Match with name */
	match = strcmp(name, xml_name(x));
	break;
    case Y_LEAF_LIST: /* Match with name and value */
	if (userorder && yang_find((yang_node*)y, Y_ORDERED_BY, "user") != NULL)
	    *userorder=1;
	if ((b=xml_body(x)) == NULL)
	    match = 1;
	else
	    match = strcmp(keyval[0], b);
	break;
    case Y_LIST: /* Match with array of key values */
	if (userorder && yang_find((yang_node*)y, Y_ORDERED_BY, "user") != NULL)
	    *userorder=1;
	/* All must match */
	for (i=0; i<keynr; i++){
	    keyname = keyvec[i];
	    key = keyval[i];
	    /* Eg return "e0" in <if><name>e0</name></name></if> given "name" */
	    if ((b = xml_find_body(x, keyname)) == NULL)
		break; /* error case */
	    if ((match = strcmp(key, b)) != 0)
		break;
	}
	break;
    default:
	break;
    }
    // done:
    return match; /* should not reach here */
}

/*! Sort children of an XML node
 * Assume populated by yang spec.
 * @param[in] x0   XML node
 * @param[in] arg  Dummy so it can be called by xml_apply()
 * @retval    -1    Error, aborted at first error encounter
 * @retval     0    OK, all nodes traversed (subparts may have been skipped)
 * @retval     1    OK, aborted on first fn returned 1
 * @see xml_apply  - typically called by recursive apply function
 */
int
xml_sort(cxobj *x,
	 void  *arg)
{
    yang_stmt *ys;

    /* Abort sort if non-config (=state) data */
    if ((ys = xml_spec(x)) != 0 && yang_config(ys)==0)
	return 1;
    qsort(xml_childvec_get(x), xml_child_nr(x), sizeof(cxobj *), xml_cmp);
    return 0;
}

/*! Special case search for ordered-by user where linear sort is used
 */
static cxobj *
xml_search_userorder(cxobj        *x0,
		     yang_stmt    *y,
		     char         *name,
		     int           yangi,
		     int           mid,
		     enum rfc_6020 keyword,   
		     int           keynr,
		     char        **keyvec,
		     char        **keyval)
{
    int    i;
    cxobj *xc;
    
    for (i=mid+1; i<xml_child_nr(x0); i++){ /* First increment */
	xc = xml_child_i(x0, i);
	y = xml_spec(xc);
	if (yangi!=yang_order(y))
	    break;
	if (xml_cmp1(xc, y, name, keyword, keynr, keyvec, keyval, NULL) == 0)
	    return xc;
    }
    for (i=mid-1; i>=0; i--){ /* Then decrement */
	xc = xml_child_i(x0, i);
	y = xml_spec(xc);
	if (yangi!=yang_order(y))
	    break;
	if (xml_cmp1(xc, y, name, keyword, keynr, keyvec, keyval, NULL) == 0)
	    return xc;
    }
    return NULL; /* Not found */
}

/*!
 * @param[in] yangi    Yang order
 * @param[in] keynr    Length of keyvec/keyval vector when applicable
 * @param[in] keyvec   Array of of yang key identifiers
 * @param[in] keyval   Array of of yang key values
 * @param[in] low      Lower bound of childvec search interval 
 * @param[in] upper    Lower bound of childvec search interval 
 */
static cxobj *
xml_search1(cxobj        *x0,
	    char         *name,
	    int           yangi,
	    enum rfc_6020 keyword,   
	    int           keynr,
	    char        **keyvec,
	    char        **keyval,
	    int           low, 
	    int           upper)
{
    int        mid;
    int        cmp;
    cxobj     *xc;
    yang_stmt *y;
    int        userorder= 0;
    
    if (upper < low)
	return NULL; /* not found */
    mid = (low + upper) / 2;
    if (mid >= xml_child_nr(x0))  /* beyond range */
	return NULL;
    xc = xml_child_i(x0, mid);
    if ((y = xml_spec(xc)) == NULL)
	return NULL;
    cmp = yangi-yang_order(y);
    if (cmp == 0){
	cmp = xml_cmp1(xc, y, name, keyword, keynr, keyvec, keyval, &userorder);
	if (userorder && cmp)	    /* Look inside this yangi order */
	    return xml_search_userorder(x0, y, name, yangi, mid, keyword, keynr, keyvec, keyval);
    }
    if (cmp == 0)
	return xc;
    else if (cmp < 0)
	return xml_search1(x0, name, yangi, keyword,
			   keynr, keyvec, keyval, low, mid-1);
    else 
	return xml_search1(x0, name, yangi, keyword,
			   keynr, keyvec, keyval, mid+1, upper);
    return NULL;
}

/*! Find XML children using binary search
 * @param[in] yangi  yang child order
 * @param[in]  keynr    Length of keyvec/keyval vector when applicable
 * @param[in]  keyvec   Array of of yang key identifiers
 * @param[in]  keyval   Array of of yang key values
 */
cxobj *
xml_search(cxobj        *x0,
	   char         *name,
	   int           yangi,
	   enum rfc_6020 keyword,   
	   int           keynr,
	   char        **keyvec,
	   char        **keyval)
{
    return xml_search1(x0, name, yangi, keyword, keynr, keyvec, keyval,
		       0, xml_child_nr(x0));
}


/*! Position where to insert xml object into a list of children nodes
 * @note EXPERIMENTAL
 * Insert after position returned
 * @param[in]  x0       XML parent node.
 * @param[in]  low       Lower bound
 * @param[in]  upper     Upper bound (+1)
 * @retval     position 
 * XXX: Problem with this is that evrything must be known before insertion
 */
int
xml_insert_pos(cxobj        *x0,
	       char         *name,
	       int           yangi,
	       enum rfc_6020 keyword,   
	       int           keynr,
	       char        **keyvec,
	       char        **keyval,
	       int           low, 
	       int           upper)
{
    int        mid;
    cxobj     *xc;
    yang_stmt *y;
    int        cmp;
    int        i;
    int        userorder= 0;
    
    if (upper < low)
	return low; /* not found */
    mid = (low + upper) / 2;
    if (mid >= xml_child_nr(x0))
	return xml_child_nr(x0); /* upper range */
    xc = xml_child_i(x0, mid); 
    y = xml_spec(xc);
    cmp = yangi-yang_order(y);
    if (cmp == 0){
	cmp = xml_cmp1(xc, y, name, keyword, keynr, keyvec, keyval, &userorder);
	if (userorder){	    /* Look inside this yangi order */
	    /* Special case: append last of equals if ordered by user */
	    for (i=mid+1;i<xml_child_nr(x0);i++){
		xc = xml_child_i(x0, i);
		if (strcmp(xml_name(xc), name))
		    break;
		mid=i; /* still ok */
	    }
	    return mid;
	}
    }
    if (cmp == 0)
	return mid;
    else if (cmp < 0)
	return xml_insert_pos(x0, name, yangi, keyword,
			      keynr, keyvec, keyval, low, mid-1);
    else
	return xml_insert_pos(x0, name, yangi, keyword,
			      keynr, keyvec, keyval, mid+1, upper);
}

/*! Find matching xml child given name and optional key values
 * container: x0, y->keyword, name
 * list:      x0, y->keyword, y->key, name
 *
 * The function needs a vector of key values (or single for LEAF_LIST).
 * What format? 
 * 1) argc/argv:: "val1","val2" <<==
 * 2) cv-list?
 * 3) va-list?
 *
 *      yc - LIST (interface) - 
 *       ^
 *       |
 * x0-->x0c-->(name=interface)+->x(name=name)->xb(value="eth0") <==this is
 *       |
 *       v
 *      x1c->name (interface)
 *      x1c->x(name=name)->xb(value="eth0")
 *
 *      CONTAINER:name
 *      LEAF:     name
 *      LEAFLIST: name/body... #b0
 *      LIST:     name/key0/key1... #b2vec+b0 -> x0c

 * <interface><name>eth0</name></interface>
 * <interface><name>eth1</name></interface>
 * <interface><name>eth2</name></interface>
 * @param[in]  x0       XML node. Find child of this node.
 * @param[in]  keyword  Yang keyword. Relevant: container, list, leaf, leaf_list
 * @param[in]  keynr    Length of keyvec/keyval vector when applicable
 * @param[in]  keyvec   Array of of yang key identifiers
 * @param[in]  keyval   Array of of yang key values
 * @param[out] xp       Return value on success, pointer to XML child node
 * @note If keyword is:
 *   - list, keyvec and keyval should be an array with keynr length
 *   - leaf_list, keyval should be 1 and keyval should contain one element
 *   - otherwise, keyval should be 0 and keyval and keyvec should be both NULL.
 */
cxobj *
xml_match(cxobj        *x0,
	  char         *name,
	  enum rfc_6020 keyword,   
	  int           keynr,
	  char        **keyvec,
	  char        **keyval)
{
    char  *key;
    char  *keyname;
    char  *b0;
    cxobj *x = NULL;
    int    equal;
    int    i;
    
    x = NULL;
    switch (keyword){
    case Y_CONTAINER: /* Match with name */
    case Y_LEAF: /* Match with name */
	if (keynr != 0){
	    clicon_err(OE_XML, EINVAL, "Expected no key argument to CONTAINER or LEAF");
	    goto ok;
	}
	x = xml_find(x0, name);
	break;
    case Y_LEAF_LIST: /* Match with name and value */
	if (keynr != 1)
	    goto ok;
	x = xml_find_body_obj(x0, name, keyval[0]);
	break;
    case Y_LIST: /* Match with array of key values */
	i = 0;
	while ((x = xml_child_each(x0, x, CX_ELMNT)) != NULL){
	    equal = 0;
	    if (strcmp(xml_name(x), name))
		continue;
	    /* Must be inner loop */
	    for (i=0; i<keynr; i++){
		keyname = keyvec[i];
		key = keyval[i];
		equal = 0;
		if ((b0 = xml_find_body(x, keyname)) == NULL)
		    break; /* error case */
		if (strcmp(b0, key))
		    break; /* stop as soon as inequal key found */
		equal=1; /* reaches here for all keynames, x is found. */
	    }
	    if (equal) /* x matches, oyherwise look for other */
		break;
	} /* while x */
	break;
    default: 
	break;
    }
 ok:
    return x;
}

/*! Verify all children of XML node are sorted according to xml_sort()
 * @param[in]   x       XML node. Check its children
 * @param[in]   arg     Dummy. Ensures xml_apply can be used with this fn
 @ @retval      0       Sorted
 @ @retval     -1       Not sorted
 * @see xml_apply
 */
int
xml_sort_verify(cxobj *x0,
		void  *arg)
{
    int    retval = -1;
    cxobj *x = NULL;
    cxobj *xprev = NULL;
    yang_stmt *ys;

    /* Abort sort if non-config (=state) data */
    if ((ys = xml_spec(x0)) != 0 && yang_config(ys)==0){
	retval = 1;
	goto done;
    }
    while ((x = xml_child_each(x0, x, -1)) != NULL) {
	if (xprev != NULL){ /* Check xprev <= x */
	    if (xml_cmp(&xprev, &x) > 0)
		goto done;
	}
	xprev = x;
    }
    retval = 0;
 done:
    return retval;
}

/*! Given child tree x1c, find matching child in base tree x0 and return as x0cp
 * @param[in]  x0      Base tree node
 * @param[in]  x1c     Modification tree child
 * @param[in]  yc      Yang spec of tree child
 * @param[out] x0cp    Matching base tree child (if any)
 * @retval     0       OK
 * @retval    -1       Error
 */
int
match_base_child(cxobj      *x0, 
		 cxobj      *x1c,
		 yang_stmt  *yc,
		 cxobj     **x0cp)
{
    int        retval = -1;
    cvec      *cvk = NULL; /* vector of index keys */
    cg_var    *cvi;
    char      *b;
    char      *keyname;
    char       keynr = 0;
    char     **keyval = NULL;
    char     **keyvec = NULL;
    int        i;
    int        yorder;
    cxobj     *x0c = NULL;
    yang_stmt *y0c;
    yang_node *y0p;
    yang_node *yp; /* yang parent */
    
    *x0cp = NULL; /* init return value */
    /* Special case is if yc parent (yp) is choice/case
     * then find x0 child with same yc even though it does not match lexically
     * However this will give another y0c != yc
     */
    if ((yp = yang_choice(yc)) != NULL){
	x0c = NULL;
	while ((x0c = xml_child_each(x0, x0c, CX_ELMNT)) != NULL) {
	    if ((y0c = xml_spec(x0c)) != NULL &&
		(y0p = yang_choice(y0c)) != NULL &&
		y0p == yp)
		break;	/* x0c will have a value */
	}
	*x0cp = x0c;
	goto ok; /* What to do if not found? */
    }
    switch (yc->ys_keyword){
    case Y_CONTAINER: 	/* Equal regardless */
    case Y_LEAF: 	/* Equal regardless */
	break;
    case Y_LEAF_LIST: /* Match with name and value */
	keynr = 1;
	if ((keyval = calloc(keynr+1, sizeof(char*))) == NULL){
	    clicon_err(OE_UNIX, errno, "calloc");
	    goto done;
	}
	if ((keyval[0] = xml_body(x1c)) == NULL)
	    goto ok;
	break;
    case Y_LIST: /* Match with key values */
	cvk = yc->ys_cvec; /* Use Y_LIST cache, see ys_populate_list() */
	/* Count number of key indexes 
	 * Then create two vectors one with names and one with values of x1c,
	 * ec: keyvec: [a,b,c]  keyval: [1,2,3]
	 */
	cvi = NULL; keynr = 0;
	while ((cvi = cvec_each(cvk, cvi)) != NULL) 
	    keynr++;
	if ((keyval = calloc(keynr+1, sizeof(char*))) == NULL){
	    clicon_err(OE_UNIX, errno, "calloc");
	    goto done;
	}
	if ((keyvec = calloc(keynr+1, sizeof(char*))) == NULL){
	    clicon_err(OE_UNIX, errno, "calloc");
	    goto done;
	}
	cvi = NULL; i = 0;
	while ((cvi = cvec_each(cvk, cvi)) != NULL) {
	    keyname = cv_string_get(cvi);
	    keyvec[i] = keyname;
	    if ((b = xml_find_body(x1c, keyname)) == NULL)
		goto ok; /* not found */
	    keyval[i++] = b;
	}
	break;
    default:
	break;
    }
    /* Get match. Sorting mode(optimized) or not?*/
    if (xml_child_nr(x0)==0 || xml_spec(xml_child_i(x0,0))!=NULL){
	yorder = yang_order(yc);
	x0c = xml_search(x0, xml_name(x1c), yorder, yc->ys_keyword, keynr, keyvec, keyval);
    }
    else{
	x0c = xml_match(x0, xml_name(x1c), yc->ys_keyword, keynr, keyvec, keyval);
    }
    *x0cp = x0c;
 ok:
    retval = 0;
 done:
    if (keyval)
	free(keyval);
    if (keyvec)
	free(keyvec);
    return retval;
}
	   
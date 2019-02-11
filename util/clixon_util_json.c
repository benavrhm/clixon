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

 * JSON support functions.
 * JSON syntax is according to:
 * http://www.ecma-international.org/publications/files/ECMA-ST/ECMA-404.pdf
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
#include <fnmatch.h>
#include <stdint.h>
#include <syslog.h>
#include <assert.h>

/* cligen */
#include <cligen/cligen.h>

/* clixon */
#include "clixon/clixon.h"

/*
 * Turn this on to get a json parse and pretty print test program
 * Usage: xpath
 * read json from input
 * Example compile:
 gcc -g -o json -I. -I../clixon ./clixon_json.c -lclixon -lcligen
 * Example run:
    echo '{"foo": -23}' | ./json
*/
static int
usage(char *argv0)
{
    fprintf(stderr, "usage:%s [options]\n"
	    "where options are\n"
            "\t-h \t\tHelp\n"
    	    "\t-D <level> \tDebug\n"
	    "\t-j \t\tOutput as JSON\n"
	    "\t-l <s|e|o> \tLog on (s)yslog, std(e)rr, std(o)ut (stderr is default)\n",
	    argv0);
    exit(0);
}

int
main(int    argc,
     char **argv)
{
    int   retval = -1;
    cxobj *xt = NULL;
    cxobj *xc;
    cbuf  *cb = cbuf_new();
    int   c;
    int   logdst = CLICON_LOG_STDERR;
    int   json = 0;
    
    optind = 1;
    opterr = 0;
    while ((c = getopt(argc, argv, "hD:jl:")) != -1)
	switch (c) {
	case 'h':
	    usage(argv[0]);
	    break;
    	case 'D':
	    if (sscanf(optarg, "%d", &debug) != 1)
		usage(argv[0]);
	    break;
	case 'j':
	    json++;
	    break;
	case 'l': /* Log destination: s|e|o|f */
	    if ((logdst = clicon_log_opt(optarg[0])) < 0)
		usage(argv[0]);
	    break;
	default:
	    usage(argv[0]);
	    break;
	}
    clicon_log_init(__FILE__, debug?LOG_DEBUG:LOG_INFO, logdst);
    if (json_parse_file(0, NULL, &xt) < 0)
	goto done;
    xc = NULL;
    while ((xc = xml_child_each(xt, xc, -1)) != NULL) 
	if (json)
	    xml2json_cbuf(cb, xc, 0); /* print xml */
	else
	    clicon_xml2cbuf(cb, xc, 0, 0); /* print xml */
    fprintf(stdout, "%s", cbuf_get(cb));
    fflush(stdout);
    retval = 0;
 done:
    if (xt)
	xml_free(xt);
    if (cb)
	cbuf_free(cb);
    return retval;
}
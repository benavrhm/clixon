#!/bin/bash
# RPC tests
# Validate parameters in restconf and netconf, check namespaces, etc
# See rfc8040 3.6
APPNAME=example

# include err() and new() functions and creates $dir
. ./lib.sh
cfg=$dir/conf.xml

# Use yang in example
cat <<EOF > $cfg
<config xmlns="urn:example:clixon">
  <CLICON_CONFIGFILE>$cfg</CLICON_CONFIGFILE>
  <CLICON_YANG_DIR>/usr/local/share/clixon</CLICON_YANG_DIR>
  <CLICON_YANG_DIR>$IETFRFC</CLICON_YANG_DIR>
  <CLICON_YANG_MODULE_MAIN>clixon-example</CLICON_YANG_MODULE_MAIN>
  <CLICON_BACKEND_DIR>/usr/local/lib/$APPNAME/backend</CLICON_BACKEND_DIR>
  <CLICON_CLISPEC_DIR>/usr/local/lib/$APPNAME/clispec</CLICON_CLISPEC_DIR>
  <CLICON_CLI_DIR>/usr/local/lib/$APPNAME/cli</CLICON_CLI_DIR>
  <CLICON_CLI_MODE>$APPNAME</CLICON_CLI_MODE>
  <CLICON_RESTCONF_PRETTY>false</CLICON_RESTCONF_PRETTY>
  <CLICON_SOCK>/usr/local/var/$APPNAME/$APPNAME.sock</CLICON_SOCK>
  <CLICON_BACKEND_PIDFILE>/usr/local/var/$APPNAME/$APPNAME.pidfile</CLICON_BACKEND_PIDFILE>
  <CLICON_CLI_GENMODEL_COMPLETION>1</CLICON_CLI_GENMODEL_COMPLETION>
  <CLICON_XMLDB_DIR>/usr/local/var/$APPNAME</CLICON_XMLDB_DIR>
  <CLICON_XMLDB_PLUGIN>/usr/local/lib/xmldb/text.so</CLICON_XMLDB_PLUGIN>
</config>
EOF

new "test params: -f $cfg"
if [ $BE -ne 0 ]; then
    new "kill old backend"
    sudo clixon_backend -zf $cfg
    if [ $? -ne 0 ]; then
	err
    fi
    new "start backend -s init -f $cfg"
    sudo $clixon_backend -s init -f $cfg -D $DBG
    if [ $? -ne 0 ]; then
	err
    fi
fi

new "kill old restconf daemon"
sudo pkill -u www-data clixon_restconf

new "start restconf daemon"
sudo su -c "$clixon_restconf -f $cfg -D $DBG" -s /bin/sh www-data &

sleep $RCWAIT

new "rpc tests"

# 1.First some positive tests vary media types
# extra complex because pattern matching on return haders
new "restconf empty rpc"
ret=$(curl -is -X POST http://localhost/restconf/operations/clixon-example:empty)
expect="204 No Content"
match=`echo $ret | grep -EZo "$expect"`
if [ -z "$match" ]; then
    err "$expect" "$ret"
fi

new "netconf empty rpc"
expecteof "$clixon_netconf -qf $cfg" 0 '<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0"><empty xmlns="urn:example:clixon"/></rpc>]]>]]>' '^<rpc-reply xmlns="urn:ietf:params:xml:ns:netconf:base:1.0"><ok/></rpc-reply>]]>]]>$'

new2 "restconf example rpc json/json default - no http media headers"
expecteq "$(curl -s -X POST -d '{"clixon-example:input":{"x":0}}' http://localhost/restconf/operations/clixon-example:example)" '{"clixon-example:output": {"x": "0","y": "42"}}
'

new2 "restconf example rpc json/json change y default"
expecteq "$(curl -s -X POST -d '{"clixon-example:input":{"x":"0","y":"99"}}' http://localhost/restconf/operations/clixon-example:example)" '{"clixon-example:output": {"x": "0","y": "99"}}
'

new2 "restconf example rpc json/json"
# XXX example:input example:output
expecteq "$(curl -s -X POST -H 'Content-Type: application/yang-data+json' -H 'Content-Type: application/yang-data+json' -d '{"clixon-example:input":{"x":"0"}}' http://localhost/restconf/operations/clixon-example:example)" '{"clixon-example:output": {"x": "0","y": "42"}}
'

new2 "restconf example rpc xml/json"
expecteq "$(curl -s -X POST -H 'Content-Type: application/yang-data+xml' -H 'Content-Type: application/yang-data+json' -d '<input xmlns="urn:example:clixon"><x>0</x></input>' http://localhost/restconf/operations/clixon-example:example)"  '{"clixon-example:output": {"x": "0","y": "42"}}
'

new2 "restconf example rpc json/xml"
expecteq "$(curl -s -X POST -H 'Content-Type: application/yang-data+json' -H 'Accept: application/yang-data+xml' -d '{"clixon-example:input":{"x":"0"}}' http://localhost/restconf/operations/clixon-example:example)" '<output xmlns="urn:example:clixon"><x>0</x><y>42</y></output>
'

new2 "restconf example rpc xml/xml"
expecteq "$(curl -s -X POST -H 'Content-Type: application/yang-data+xml' -H 'Accept: application/yang-data+xml' -d '<input xmlns="urn:example:clixon"><x>0</x></input>' http://localhost/restconf/operations/clixon-example:example)" '<output xmlns="urn:example:clixon"><x>0</x><y>42</y></output>
'

new "netconf example rpc"
expecteof "$clixon_netconf -qf $cfg" 0 '<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0"><example xmlns="urn:example:clixon"><x>0</x></example></rpc>]]>]]>' '^<rpc-reply xmlns="urn:ietf:params:xml:ns:netconf:base:1.0"><x xmlns="urn:example:clixon">0</x><y xmlns="urn:example:clixon">42</y></rpc-reply>]]>]]>$'

# 2. Then error cases
#
new "restconf empy rpc with null input"
ret=$(curl -is -X POST -d '{"clixon-example:input":null}' http://localhost/restconf/operations/clixon-example:empty)
expect="204 No Content"
match=`echo $ret | grep -EZo "$expect"`
if [ -z "$match" ]; then
    err "$expect" "$ret"
fi

new2 "restconf empy rpc with input x"
expecteq "$(curl -s -X POST -d '{"clixon-example:input":{"x":0}}' http://localhost/restconf/operations/clixon-example:empty)" '{"ietf-restconf:errors" : {"error": {"error-type": "application","error-tag": "unknown-element","error-info": {"bad-element": "x"},"error-severity": "error"}}}'

new2 "restconf omit mandatory"
expecteq "$(curl -s -X POST -d '{"clixon-example:input":null}' http://localhost/restconf/operations/clixon-example:example)" '{"ietf-restconf:errors" : {"error": {"error-type": "application","error-tag": "missing-element","error-info": {"bad-element": "x"},"error-severity": "error","error-message": "Mandatory variable"}}}'

new2 "restconf add extra"
expecteq "$(curl -s -X POST -d '{"clixon-example:input":{"x":"0","extra":"0"}}' http://localhost/restconf/operations/clixon-example:example)" '{"ietf-restconf:errors" : {"error": {"error-type": "application","error-tag": "unknown-element","error-info": {"bad-element": "extra"},"error-severity": "error"}}}'

new2 "restconf wrong method"
expecteq "$(curl -s -X POST -d '{"clixon-example:input":{"x":"0"}}' http://localhost/restconf/operations/clixon-example:wrong)" '{"ietf-restconf:errors" : {"error": {"error-type": "application","error-tag": "missing-element","error-info": {"bad-element": "wrong"},"error-severity": "error","error-message": "RPC not defined"}}}'

new2 "restconf example missing input"
expecteq "$(curl -s -X POST -d '{"clixon-example:input":null}' http://localhost/restconf/operations/ietf-netconf:edit-config)" '{"ietf-restconf:errors" : {"error": {"error-type": "protocol","error-tag": "operation-failed","error-severity": "error","error-message": "yang module not found"}}}'


new "netconf kill-session missing session-id mandatory"
expecteof "$clixon_netconf -qf $cfg" 0 '<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0"><kill-session/></rpc>]]>]]>' '^<rpc-reply><rpc-error><error-type>application</error-type><error-tag>missing-element</error-tag><error-info><bad-element>session-id</bad-element></error-info><error-severity>error</error-severity><error-message>Mandatory variable</error-message></rpc-error></rpc-reply>]]>]]>$'

new "netconf edit-config ok"
expecteof "$clixon_netconf -qf $cfg" 0 '<rpc><edit-config xmlns="urn:ietf:params:xml:ns:netconf:base:1.0"><target><candidate/></target><config/></edit-config></rpc>]]>]]>' "^<rpc-reply><ok/></rpc-reply>]]>]]>$"

new "netconf edit-config extra arg should fail"
expecteof "$clixon_netconf -qf $cfg" 0 '<rpc><edit-config xmlns="urn:ietf:params:xml:ns:netconf:base:1.0"><target><candidate/></target><extra/><config/></edit-config></rpc>]]>]]>' "^<rpc-reply><rpc-error><error-type>application</error-type><error-tag>unknown-element</error-tag><error-info><bad-element>extra</bad-element></error-info><error-severity>error</error-severity></rpc-error></rpc-reply>]]>]]>$"

new "netconf edit-config empty target should fail"
expecteof "$clixon_netconf -qf $cfg" 0 '<rpc><edit-config xmlns="urn:ietf:params:xml:ns:netconf:base:1.0"><target/><config/></edit-config></rpc>]]>]]>' '^<rpc-reply><rpc-error><error-type>application</error-type><error-tag>data-missing</error-tag><error-app-tag>missing-choice</error-app-tag><error-info><missing-choice>config-target</missing-choice></error-info><error-severity>error</error-severity></rpc-error></rpc-reply>]]>]]>$'

new "netconf edit-config missing target should fail"
expecteof "$clixon_netconf -qf $cfg" 0 '<rpc><edit-config xmlns="urn:ietf:params:xml:ns:netconf:base:1.0"><config/></edit-config></rpc>]]>]]>' '^<rpc-reply><rpc-error><error-type>application</error-type><error-tag>missing-element</error-tag><error-info><bad-element>target</bad-element></error-info><error-severity>error</error-severity><error-message>Mandatory variable</error-message></rpc-error></rpc-reply>]]>]]>$'

new "netconf edit-config missing config should fail"
expecteof "$clixon_netconf -qf $cfg" 0 '<rpc><edit-config xmlns="urn:ietf:params:xml:ns:netconf:base:1.0"><target><candidate/></target></edit-config></rpc>]]>]]>' '^<rpc-reply><rpc-error><error-type>application</error-type><error-tag>data-missing</error-tag><error-app-tag>missing-choice</error-app-tag><error-info><missing-choice>edit-content</missing-choice></error-info><error-severity>error</error-severity></rpc-error></rpc-reply>]]>]]>$'

new "Kill restconf daemon"
sudo pkill -u www-data -f "/www-data/clixon_restconf"

if [ $BE -eq 0 ]; then
    exit # BE
fi

new "Kill backend"
# Check if premature kill
pid=`pgrep -u root -f clixon_backend`
if [ -z "$pid" ]; then
    err "backend already dead"
fi
# kill backend
sudo clixon_backend -z -f $cfg
if [ $? -ne 0 ]; then
    err "kill backend"
fi

rm -rf $dir

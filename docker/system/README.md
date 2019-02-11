# Clixon example container

This directory show how to build a "monolithic" clixon docker
container exporting port 80 and contains the example application with
both restconf, netconf, cli and backend.

Note that the container is built from scratch instead of using the
base image. And it differs from the base image in that it builds the
code from the local git locally (regardless of status and branch)
wheras the base image pulls from remote github master branch.

Note that it could use the base image, and may in the future, see
comments in the Dockerfile.

The directory contains the following files:
	 cleanup.sh     kill containers
	 Dockerfile     Docker build instructions
	 Makefile.in    "make docker" builds the container
	 README.md	This file
	 start.sh       Start containers
	 startsystem.sh Internal start script copied to inside the container (dont run from shell)

How to build and start the container (called clixon-system):
```
  $ make docker
  $ ./start.sh 
```

The start.sh has a number of environment variables to alter the default behaviour:
* PORT - Nginx exposes port 80 per default. Set `PORT=8080` for example to access restconf using 8080.
* DBG - Set debug. The clixon_backend will be shown on docker logs.
* CONFIG - Set XML configuration file other than the default example.
* STORE - Set running datastore content to other than default.

Example:
```
  $ DBG=1 PORT=8080 ./start.sh
```

Once running you can access it as follows:
* CLI: `sudo docker exec -it clixon-system clixon_cli`
* Netconf: `sudo docker exec -it clixon-system clixon_netconf`
* Restconf: `curl -G http://localhost/restconf`
* Run tests: `sudo docker exec -it clixon-system bash -c 'cd /clixon/clixon/test; exec ./all.sh'`

To check status and then kill it:
```
  $ sudo docker ps --all
  $ ./cleanup.sh 
```
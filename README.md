ib-dpdk
=============

DPDK-like functions for InfiniBand.

To run an ib-dpdk standalone test:

0. Create 512 hugepages on all experiment machines.  The HRD library 
uses hugepages for its SEND and RECV regions.  Use the hugepages-create.sh 
script in the scripts folder:
	./hugepages-create.sh 0 512

1. Update HRD_REGISTRY_IP in hrd.h and hrd_registry_ip in run_servers.sh
to the IP address of a machine running a memcached server.

2. At all server machines, run run-servers.sh.  As the server threads will
register their QPs with the central memcached server, wait for this to
complete.

3. At all client machines, run run-machine.sh.  The clients will get the
server's QP identifier from the central memcached server.  Any number of 
client machines can be added to the server this way.

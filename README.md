rdma-dpdk
=========

This framework provides [DPDK](http://dpdk.org)-like functions over RDMA.
Mellanox has released a [DPDK PMD](http://dpdk.org/doc/guides/nics/mlx4.html),
but it supports only ConnectX-3 Ethernet NICs (not InfiniBand). This library
ideally works with **all** RDMA NICs. It has been tested with ConnectX-3 and
Connect-IB InfiniBand NICs.

## Required libraries and software
 * Mellanox OFED 2.4+
 * memcached, libmemcached-dev, libmemcached-tools
 * libnuma-dev
 * Ubuntu 12.04+

## Design

1. `rdma-dpdk` performs all communication using SEND/RECV verbs over RDMA's
UD transport. This provides performance that is comparable to an RDMA
WRITE-based implementation, but is more scalable. Read our
[paper](http://www.cs.cmu.edu/~akalia/doc/sigcomm14/herd_readable.pdf) for
more details.

2. `rdma-dpdk` uses a memcached instance as a central registry to store
information about QPs. This registry is only contacted during connection setup;
`rdma-dpdk` is not limited by memcached's performance.

## Example run

Follow these instructions to run the provided example program (`main.c`). This
program runs multiple server threads on 1 server machine, and multiple client
threads on possibly many client machines. Client threads send messages to the
server threads. On receiving a message from a client, the server thread sends
a message back to it.

1. Increase Linux's limit on the amount of shared memory that a process is
allowed to use. From the scripts folder:
```
    ./shm-incr.sh
```

2. Create 512 hugepages on all experiment machines.  The HRD library 
uses hugepages for its SEND and RECV regions. From the scrips folder:
```
    ./hugepages-create.sh 0 512
```

3. At all machines, set the `HRD_REGISTRY_IP` environment variable to the IP
address or hostname of the server machine.

4. At the server machine, run `run-servers.sh`.  The server threads will
register their QPs with the registry.

5. At client machine `i`, run `run-machine.sh i`.  The clients will get the
servers' QP identifiers from the registry.  Any number of client machines can
be added to the server this way.


rdma-dpdk
=========

DPDK-like functions over RMDA.

## Required libraries and software
 * RDMA drivers (e.g., Mellanox OFED for Mellanox HCAs)
 * memcached
 * libmemcached-dev
 * libnuma-dev

## Design
1. `rdma-dpdk` performs all communication using SEND/RECV verbs over RDMA's UD transport. This provides performance that is comparable to an RDMA WRITE-based implementation, but is more scalable. Read our [paper](http://www.cs.cmu.edu/~akalia/doc/sigcomm14/herd_readable.pdf) for more details.
2. `rdma-dpdk` uses a memcached instance as a central registry to store information about QPs. This registry is only contacted during connection setup; `rdma-dpdk` is not limited by memcached's performance.

## Example run

1. Create 512 hugepages on all experiment machines.  The HRD library 
uses hugepages for its SEND and RECV regions. From the scrips folder:
```
    ./hugepages-create.sh 0 512
```

2. Update `HRD_REGISTRY_IP` in `hrd.h` and `hrd_registry_ip` in `run_servers.sh`
to the IP address of a machine running a memcached server. Run memcached using
```
    ./memcached -l 0.0.0.0`
 ```

3. At all server machines, run `run-servers.sh`.  The server threads will
register their QPs with the central memcached server; wait for this to
complete.

4. At all client machines, run `run-machine.sh`.  The clients will get the
servers' QP identifiers from the central memcached server.  Any number of 
client machines can be added to the server this way.


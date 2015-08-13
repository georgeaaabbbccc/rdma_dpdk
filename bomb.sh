for i in `seq 2 60`; do
	ssh -oStrictHostKeyChecking=no node-$i.RDMA.fawn.apt.emulab.net "cd mica-intel/ib-dpdk; ./kill.sh" &
done

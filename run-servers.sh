# A function to echo in blue color
function blue() {
	es=`tput setaf 4`
	ee=`tput sgr0`
	echo "${es}$1${ee}"
}

blue "Removing hugepages"
shm-rm.sh 1>/dev/null 2>/dev/null

num_server_threads=10
num_client_machines=1
registry_ip="128.2.211.34"

blue "Reset server registry"
# This ssh needs to be synchronous
ssh -oStrictHostKeyChecking=no $registry_ip "
	touch servers;
	memcflush --servers=localhost;
	memccp --servers=localhost servers"

blue "Starting $num_server_threads server threads"

sudo LD_LIBRARY_PATH=/usr/local/lib/ ./main $num_server_threads server &

exit
sleep 1

for i in `seq 1 $num_client_machines`; do
	blue "Starting client $client_id"
	mc=`expr $i + 1`
	client_id=`expr $mc - 2`
	ssh -oStrictHostKeyChecking=no node-$mc.RDMA.fawn.apt.emulab.net "
		cd mica-intel/ib-dpdk; 
		./run-remote.sh $client_id" &
	sleep .5
done

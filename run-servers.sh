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
: ${HRD_REGISTRY_IP:?"Need to set HRD_REGISTRY_IP non-empty"}

# Check if memccp and memcached are installed
if ! hash memccp 2>/dev/null; then
	blue "Please install memccp"
	exit
fi

if ! hash memcached 2>/dev/null; then
	blue "Please install memcached"
	exit
fi

blue "Reset QP registry"
sudo killall memcached
memcached -l 0.0.0.0 &
sleep 1	# Wait for memcached to initialize before inserting a key
touch rdma_dpdk_servers

# This inserts a mapping <"rdma_dpdk_servers" -> NULL> into the memcached
# instance. Now we can append to key "rdma_dpdk_servers".
memccp --servers=localhost rdma_dpdk_servers

blue "Starting $num_server_threads server threads"

sudo -E LD_LIBRARY_PATH=/usr/local/lib/ ./main $num_server_threads server &


# A function to echo in blue color
function blue() {
	es=`tput setaf 4`
	ee=`tput sgr0`
	echo "${es}$1${ee}"
}

shm-rm.sh 1>/dev/null 2>/dev/null

num_threads=10			#processes per client machine

blue "Running $num_threads client threads"

sudo -E LD_LIBRARY_PATH=/usr/local/lib/ ./main $num_threads &

# A function to echo in blue color
function blue() {
	es=`tput setaf 4`
	ee=`tput sgr0`
	echo "${es}$1${ee}"
}

blue "Run this script as root (sudo -s)"

# Increase the amount of per-process shared memory (Linux default is 32 MB)
echo "kernel.shmmax = 9223372036854775807" >> /etc/sysctl.conf
echo "kernel.shmall = 1152921504606846720" >> /etc/sysctl.conf
sysctl -p /etc/sysctl.conf


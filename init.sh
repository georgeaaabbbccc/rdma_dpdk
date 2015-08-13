#Run as user akalia
#For circular log and lossy index
sudo sysctl -w kernel.shmmax=2147483648		#Bytes
sudo sysctl -w kernel.shmall=2147483648		#Pages

sudo sysctl -p /etc/sysctl.conf

sudo ifconfig ib0 10.0.0.50
for i in `seq 2 20`; do
	ip=`expr 50 + $i`
	ssh anuj$i.RDMA.fawn.susitna.pdl.cmu.local "sudo ifconfig ib0 10.0.0.$ip"
done

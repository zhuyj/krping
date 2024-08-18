KSRC=/lib/modules/`uname -r`/build
KOBJ=/lib/modules/`uname -r`/build


obj-m += rdma_krperf.o
rdma_krperf-y			:= getopt.o krperf.o krperf_srq.o

default:
	make -C $(KSRC) M=`pwd` modules

install:
	make -C $(KSRC) M=`pwd` modules_install
	depmod -a

clean:
	rm -f *.o
	rm -f *.ko
	rm -f rdma_krperf.mod.c rdma_krperf.mod
	rm -f Module.symvers
	rm -f Module.markers

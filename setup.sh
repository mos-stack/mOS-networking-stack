#! /bin/bash

############################ Common Utilities ################################
FL_PCAP=1
FL_DPDK=2
FL_NETMAP=3

# vercomp returns 0 (=), 1 (>), or 2 (<)
vercomp () {
    if [[ $1 == $2 ]]
    then
        return 0
    fi
    local IFS=.
    local i ver1=($1) ver2=($2)
    # fill empty fields in ver1 with zeros
    for ((i=${#ver1[@]}; i<${#ver2[@]}; i++))
    do
        ver1[i]=0
    done
    for ((i=0; i<${#ver1[@]}; i++))
    do
        if [[ -z ${ver2[i]} ]]
        then
            # fill empty fields in ver2 with zeros
            ver2[i]=0
        fi
        if ((10#${ver1[i]} > 10#${ver2[i]}))
        then
            return 1
        fi
        if ((10#${ver1[i]} < 10#${ver2[i]}))
        then
            return 2
        fi
    done
    return 0
}

# valid_ip_addr returns 1 for valid ip address
valid_ip_addr()
{
    local  ip=$1
    local  stat=1

    if [[ $ip =~ ^[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}$ ]]; then
        OIFS=$IFS
        IFS='.'
        ip=($ip)
        IFS=$OIFS
        [[ ${ip[0]} -le 255 && ${ip[1]} -le 255 \
            && ${ip[2]} -le 255 && ${ip[3]} -le 255 ]]
        stat=$?
    fi
    return $stat
}

# Create a mos.conf file
create_config()
{
    cd $CUR_DIR
    mkdir -p $2
    CONF_FILE=$2/mos.conf
    CONF_MASTER_FILE=$2/mos-master.conf
    CONF_SLAVE_FILE=$2/mos-slave.conf
    
    echo
    echo "----------------------------------------"
    echo "Creating mos.conf file in $2"
    echo "----------------------------------------"

    if [ -f $CONF_FILE ]; then
	echo
	echo "The config file $2/mos.conf already exists."
	echo "Skip writing a new config file.."
	return		
    fi
    
    # Get the number of CPUs and translate it into CPU mask 
    let "z=2**$(nproc)-1"
    CPU_MASK=$(printf '0x%04X\n' $z)

    # Get # of CPU sockets
    let "sockets=$(cat /proc/cpuinfo | grep "physical id" | awk '{print $4}' | sort -ur | head -n 1) + 1"

    # Get the number of memory channels
    command -v dmidecode >/dev/null &&
    let "NUM_MEMCH=($(sudo dmidecode -t 17 | grep -c 'Size:')-$(sudo dmidecode -t 17 | grep -c 'Size: No Module'))/$sockets" ||
    { echo "dmidecode command not found. setting num_mem_ch = 4"; let "NUM_MEMCH=4"; }
    
    # Get the number of interfaces
    counter=0
    if [[ $1 == $FL_DPDK ]]; then
	cd /sys/module/igb_uio/drivers/pci:igb_uio/
	DEV_NAME="dpdk"
    elif [[ ( $1 != $FL_PCAP ) && ( $1 != $FL_NETMAP ) ]]; then
	echo "invalid function call 1"
	exit 1
    fi
    for i in *
    do
	if [[ $i == *":"* ]]
	then
	    let "counter=$counter + 1"
	fi
    done
    cd $CUR_DIR

    # Write up a new configuration file
    if [[ $1 == $FL_PCAP ]]; then
	cat .standalone-template.conf > $CONF_FILE
	DEVLIST=`ifconfig -s | grep -Eo '^[^ ]+' | tail -n+2`
	for dev in $DEVLIST; do
	    printf "Found $dev. Press y to select [y/N]: "
	    read option
	    if [[ "$option" == y* ]]; then
		DEVICE="\n\t\t$dev"		    
		DEVICEMASK="__coremask__devicemask"
		FMT=$(printf 's/__devicemask/%s %s/g' $DEVICE $DEVICEMASK)
		sed -i -e "$FMT" $CONF_FILE

		DEVICE="$dev""__devicelist"
		FMT=$(printf 's/__devicelist/ %s/g' $DEVICE)
		sed -i -e "$FMT" $CONF_FILE
	    fi
	done
	sed -i -e 's/__devicemask//g' $CONF_FILE
	sed -i -e 's/__devicelist//g' $CONF_FILE
	sed -i -e 's/__coremask/0x0001/g' $CONF_FILE
	sed -i -e 's/__num_memch//g' $CONF_FILE
	sed -i -e 's/__forward/1/g' $CONF_FILE
	sed -i -e 's/__multiprocess//g' $CONF_FILE

    elif [[ $1 == $FL_NETMAP ]]; then
	if [ "$3" = "epserver" ] || [ "$3" = "epwget" ] ; then
	    cat .end-template.conf > $CONF_FILE
	else
	    cat .standalone-template.conf > $CONF_FILE
	fi

	DEVLIST=`ifconfig -s | grep -Eo '^[^ ]+' | tail -n+2`
	for dev in $DEVLIST; do
	    printf "Found $dev. Press y to select [y/N]: "
	    read option
	    if [[ "$option" == y* ]]; then
		DEVICE="\n\t\t$dev"		    
		DEVICEMASK="__coremask__devicemask"
		FMT=$(printf 's/__devicemask/%s %s/g' $DEVICE $DEVICEMASK)
		sed -i -e "$FMT" $CONF_FILE

		DEVICE="$dev""__devicelist"
		FMT=$(printf 's/__devicelist/ %s/g' $DEVICE)
		sed -i -e "$FMT" $CONF_FILE
	    fi
	done
	sed -i -e 's/__devicemask//g' $CONF_FILE
	sed -i -e 's/__devicelist//g' $CONF_FILE
	sed -i -e 's/__coremask/'$CPU_MASK'/g' $CONF_FILE
	sed -i -e 's/__num_memch//g' $CONF_FILE
	sed -i -e 's/__forward/1/g' $CONF_FILE	
	sed -i -e 's/__app/'$3'/g' $CONF_FILE
	sed -i -e 's/__multiprocess//g' $CONF_FILE

	if [ "$3" = "nat" ] ; then
	    sed -i -e 's/\# tcp_tw_interval = 30/tcp_tw_interval = 30/g' $CONF_FILE
	fi    	
    else
	if [ "$3" = "epserver" ] || [ "$3" = "epwget" ] ; then
	    cat .end-template.conf > $CONF_FILE
	    cat .end-template.conf > $CONF_MASTER_FILE
	    cat .end-template.conf > $CONF_SLAVE_FILE
	else
	    cat .standalone-template.conf > $CONF_FILE
	fi
	start=0
	while [ $start -lt $counter ]
	do
	    DEVICE="\n\t\t$DEV_NAME$(($start))" 
	    DEVICEMASK="__coremask__devicemask"
	    FMT=$(printf 's/__devicemask/%s %s/g' $DEVICE $DEVICEMASK)
	    sed -i -e "$FMT" $CONF_FILE

	    DEVICE="$DEV_NAME$(($start))__devicelist"
	    FMT=$(printf 's/__devicelist/ %s/g' $DEVICE)
	    sed -i -e "$FMT" $CONF_FILE

	    let "start=$start + 1"
	done
	sed -i -e 's/__devicemask//g' $CONF_FILE
	sed -i -e 's/__devicelist//g' $CONF_FILE
	sed -i -e 's/__coremask/'$CPU_MASK'/g' $CONF_FILE
	sed -i -e 's/__forward/1/g' $CONF_FILE
	sed -i -e 's/__app/'$3'/g' $CONF_FILE

	if [ "$3" = "nat" ] ; then
	    sed -i -e 's/\# tcp_tw_interval = 30/tcp_tw_interval = 30/g' $CONF_FILE
	fi

	if [[ $1 == $FL_DPDK ]]; then
	    FMT=$(printf 's/__num_memch/%snb_mem_channels = %d%s/g' '# number of memory channels per socket [mandatory for DPDK]\n\t' $NUM_MEMCH '\n')
	    sed -i -e "$FMT" $CONF_FILE
	if [ "$3" = "epserver" ] || [ "$3" = "epwget" ] ; then
	    cp $CONF_FILE $CONF_MASTER_FILE
	    cp $CONF_FILE $CONF_SLAVE_FILE
	    sed -i -e 's/__multiprocess/multiprocess = 0 master/g' $CONF_MASTER_FILE
	    sed -i -e 's/__multiprocess/multiprocess = slave/g' $CONF_SLAVE_FILE
	fi
	else
	    echo "invalid function call 2"
	    exit 1
	fi

	sed -i -e 's/__multiprocess//g' $CONF_FILE	
    fi
    echo
    cat $CONF_FILE
}

create_makefile()
{
    cd $CUR_DIR/samples
    for d in * ; do
	if [ -f $CUR_DIR/samples/$d/Makefile.in ]; then						
	    cp $CUR_DIR/samples/$d/Makefile.in $CUR_DIR/samples/$d/Makefile
	    if [[ $1 == $FL_DPDK ]]; then
		FMT=$(printf 's/__IO_LIB_ARGS/LIBS    += -m64 -g -pthread -lrt -march=native -Wl,-export-dynamic -L..\/..\/drivers\/dpdk\/lib -Wl,-lnuma -Wl,-lmtcp -Wl,-lpthread -Wl,-lrt -Wl,-ldl -Wl,$(shell cat ..\/..\/drivers\/dpdk\/lib\/ldflags.txt)/g')
	    elif [[ $1 == $FL_PCAP ]]; then
		FMT=$(printf 's/__IO_LIB_ARGS/GCC_OPT += -D__thread="" -DBE_RESILIENT_TO_PACKET_DROP\\nINC += -DENABLE_PCAP\\nLIBS += -lpcap/g')
	    elif [[ $1 == $FL_NETMAP ]]; then
		FMT=$(printf 's/__IO_LIB_ARGS/GCC_OPT += -DENABLE_NETMAP/g')
	    fi
	    sed -i -e "$FMT" $CUR_DIR/samples/$d/Makefile
	fi
    done
}

# Setup MOS library
setup_mos_library() {
    echo "Start building up the MOS library"
    cd $CUR_DIR

    if [[ $1 == $FL_DPDK ]]; then
	mkdir -p $DPDK_DIR
	rmdir $DPDK_DIR/include $DPDK_DIR/lib 2> /dev/null
	rm $DPDK_DIR/include $DPDK_DIR/lib 2> /dev/null
	ln -s $CUR_DIR/$RTE_SDK/$RTE_TARGET/include $DPDK_DIR/include
	ln -s $CUR_DIR/$RTE_SDK/$RTE_TARGET/lib $DPDK_DIR/lib
    	cd $CUR_DIR/scripts/
	./configure --enable-dpdk
    elif [[ $1 == $FL_PCAP ]]; then
    	cd $CUR_DIR/scripts/
	./configure --enable-pcap
    elif [[ $1 == $FL_NETMAP ]]; then
    	cd $CUR_DIR/scripts/
	./configure --enable-netmap
    fi
    cd $CUR_DIR/core/src
    make clean;make
    cd $CUR_DIR
    create_makefile $1
    echo    
    echo "----------------------------------------------------------"
    echo "Done with MOS library setup"
    echo "----------------------------------------------------------"
    echo
}

#############################################################################

############################### DPDK Utilities ##############################
# Creates hugepage filesystem.
create_mnt_huge()
{
    echo "Creating /mnt/huge and mounting as hugetlbfs"
    sudo mkdir -p /mnt/huge

    grep -s '/mnt/huge' /proc/mounts > /dev/null
    if [ $? -ne 0 ] ; then
	sudo mount -t hugetlbfs nodev /mnt/huge
    fi
}

# Removes hugepage filesystem.
remove_mnt_huge()
{
    echo "Unmounting /mnt/huge and removing directory"
    grep -s '/mnt/huge' /proc/mounts > /dev/null
    if [ $? -eq 0 ] ; then
	sudo umount /mnt/huge
    fi

    if [ -d /mnt/huge ] ; then
	sudo rm -R /mnt/huge
    fi
}

# Unloads igb_uio.ko.
remove_igb_uio_module()
{
    echo "Unloading any existing DPDK UIO module"
    /sbin/lsmod | grep -s igb_uio > /dev/null
    if [ $? -eq 0 ] ; then
	sudo /sbin/rmmod igb_uio
    fi
}

# Loads new igb_uio.ko (and uio module if needed).
load_igb_uio_module()
{
    if [ ! -f $RTE_SDK/$RTE_TARGET/kmod/igb_uio.ko ];then
	echo "## ERROR: Target does not have the DPDK UIO Kernel Module."
	echo "       To fix, please try to rebuild target."
	return
    fi

    remove_igb_uio_module

    /sbin/lsmod | grep -s uio > /dev/null
    if [ $? -ne 0 ] ; then
	modinfo uio > /dev/null
	if [ $? -eq 0 ]; then
	    echo "Loading uio module"
	    sudo /sbin/modprobe uio
	fi
    fi

    # UIO may be compiled into kernel, so it may not be an error if it can't
    # be loaded.
    echo "Loading DPDK UIO module"
    sudo /sbin/insmod $RTE_SDK/$RTE_TARGET/kmod/igb_uio.ko
    if [ $? -ne 0 ] ; then
	echo "## ERROR: Could not load kmod/igb_uio.ko."
	quit
    fi
}

# Removes all reserved hugepages.
clear_numa_pages()
{
    echo > .echo_tmp
    for d in /sys/devices/system/node/node? ; do
	echo "echo 0 > $d/hugepages/hugepages-2048kB/nr_hugepages" >> .echo_tmp
    done
    echo "Removing currently reserved hugepages"
    sudo sh .echo_tmp
    rm -f .echo_tmp

    remove_mnt_huge
}
# Removes all reserved hugepages.
clear_non_numa_pages()
{
    echo > .echo_tmp
	echo "echo 0 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages" > .echo_tmp
    echo "Removing currently reserved hugepages"
    sudo sh .echo_tmp
    rm -f .echo_tmp
    remove_mnt_huge
}

#
# Creates hugepages.
#
set_non_numa_pages_default()
{
	clear_non_numa_pages
    echo > .echo_tmp
	echo "Number of pages : 1024"

	Pages=1024
	echo "echo $Pages > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages" >> .echo_tmp

    echo "Reserving hugepages"
    sudo sh .echo_tmp
    rm -f .echo_tmp

    create_mnt_huge
}

#
# Creates hugepages.
#
set_non_numa_pages()
{

	clear_non_numa_pages

	echo ""
	echo "  Input the number of 2MB pages"
	echo "  Example: to have 128MB of hugepages available, enter '64' to"
	echo "  reserve 64 * 2MB pages"
	echo -n "Number of pages: "
	read Pages

	echo "echo $Pages > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages" > .echo_tmp

	echo "Reserving hugepages"
	sudo sh .echo_tmp
	rm -f .echo_tmp

	create_mnt_huge

}

# Creates hugepages on specific NUMA nodes.
set_numa_pages_default()
{
	clear_numa_pages
    echo > .echo_tmp
    for d in /sys/devices/system/node/node? ; do
	node=$(basename $d)
	echo "Number of pages for $node: 1024"
	Pages=1024
	echo "echo $Pages > $d/hugepages/hugepages-2048kB/nr_hugepages" >> .echo_tmp
    done
    echo "Reserving hugepages"
    sudo sh .echo_tmp
    rm -f .echo_tmp

    create_mnt_huge
}

# Creates hugepages on specific NUMA nodes.
set_numa_pages()
{
	clear_numa_pages
    echo ""
    echo "  Input the number of 2MB pages for each node"
    echo "  Example: to have 128MB of hugepages available per node,"
    echo "  enter '64' to reserve 64 * 2MB pages on each node"

    echo > .echo_tmp
    for d in /sys/devices/system/node/node? ; do
	node=$(basename $d)
	echo -n "Number of pages for $node: "		
	read Pages
	echo "echo $Pages > $d/hugepages/hugepages-2048kB/nr_hugepages" >> .echo_tmp
    done
    echo "Reserving hugepages"
    sudo sh .echo_tmp
    rm -f .echo_tmp

    create_mnt_huge
}

# Uses dpdk-devbind.py to move devices to work with igb_uio
bind_nics_to_igb_uio()
{
    if  /sbin/lsmod  | grep -q igb_uio ; then
	${RTE_SDK}/tools/dpdk-devbind.py --status
	echo ""
	echo "Enter PCI address of device(s) to bind to IGB UIO driver (e.g., \"04:00.0 04:00.1\")."
	echo -n "> "
	read PCI_PATH


	sudo ${RTE_SDK}/tools/dpdk-devbind.py -b igb_uio $PCI_PATH && echo "OK"
    else
	echo "# Please load the 'igb_uio' kernel module before querying or "
	echo "# adjusting NIC device bindings"
    fi
}

#
# Uses dpdk-devbind.py to move devices to work with kernel drivers again
#
unbind_nics()
{
    DEVLIST=`ifconfig -s | grep -Eo '^[^ ]+' | tail -n+2`
    for dev in $DEVLIST
    do
	echo $dev
	if [[ "$dev" == dpdk* ]]; then
	    sudo ifconfig $dev down
	fi
    done

    ${RTE_SDK}/tools/dpdk-devbind.py --status
    echo ""
    echo -n "Enter PCI address of device to unbind: "
    read PCI_PATH
    echo ""
    echo -n "Enter name of kernel driver to bind the device to: "
    read DRV
    sudo ${RTE_SDK}/tools/dpdk-devbind.py -b $DRV $PCI_PATH && echo "OK"
}

# Brings up the interface of DPDK devices up
setup_iface_dpdk()
{
    # Create & configure /dev/dpdk-iface
    sudo rm -rf /dev/dpdk-iface
    sudo mknod /dev/dpdk-iface c 1110 0
    sudo chmod 666 /dev/dpdk-iface
    
    # First check whether igb_uio module is already loaded
    MODULE="igb_uio"
    
    if sudo lsmod | grep "$MODULE" &> /dev/null ; then
	echo "$MODULE is loaded!"
    else
	echo "$MODULE is not loaded!"
	exit 1
    fi
    
    # Next check how many devices are there in the system
    counter=0
    cd /sys/module/igb_uio/drivers/pci:igb_uio/
    for i in *
    do
	if [[ $i == *":"* ]]
	then	    
	    let "counter=$counter + 1"
	fi
    done
    cd $CUR_DIR
    
    iter=0
    # Configure each device (single-process version)
    while [ $iter -lt $counter ]
    do
	while [ 1 ]; do	
	    echo
	    echo "[dpdk$(($iter))] enter IP address[/mask] (e.g., 10.0.$iter.9[/24])"
	    echo -n "> "
	    read line
	    ip_addr=`echo $line | awk -F '/' '{print $1}'`
	    mask=`echo $line | awk -F '/' '{print $2}'`
	    valid_ip_addr $ip_addr
	    if [ $? -eq 0 ]; then
		break
	    fi
	    echo "invalid IP address!" # continue
	done
	if [ "$mask" == "" ];then
	    echo "sudo /sbin/ifconfig dpdk$(($iter)) $ip_addr up"
	    sudo /sbin/ifconfig dpdk$(($iter)) $ip_addr up
	else
	    echo "sudo /sbin/ifconfig dpdk$(($iter)) $ip_addr/$mask up"
	    sudo /sbin/ifconfig dpdk$(($iter)) $ip_addr/$mask up
	fi
	let "iter=$iter + 1"
    done
}
#############################################################################

############################### DPDK ########################################
step0_func_dpdk()
{
	if [ -d "/sys/devices/system/node" ]; then
		set_numa_pages_default
	else
		set_non_numa_pages_default
	fi

    step2_func_dpdk
    step3_func_dpdk
    step4_func_dpdk
}
step1_func_dpdk()
{
	if [ -d "/sys/devices/system/node" ]; then
		set_numa_pages
	else
		set_non_numa_pages
	fi
}
step2_func_dpdk()
{
    load_igb_uio_module
    bind_nics_to_igb_uio    
}
step3_func_dpdk()
{
    setup_iface_dpdk $last_octet
}
step4_func_dpdk()
{
    cd $CUR_DIR/samples
    for d in * ; do
	create_config $FL_DPDK "samples/$d/config" $d
    done
    echo
    echo "------------------------------------------------"
    echo "Done with configuration file setup."
    echo "Use the arp command to add static ARP entries"
    echo "------------------------------------------------"
}
step5_func_dpdk()
{
    unbind_nics
}
step6_func_dpdk()
{
    echo
    exit 1
}
#############################################################################

############################### PCAP ########################################
step0_func_pcap()
{
    cd $CUR_DIR/samples
    for d in * ; do
	if [ -d $CUR_DIR/samples/$d/config ]; then
	    create_config $FL_PCAP "samples/$d/config" $d
	fi
    done
    echo
    echo "------------------------------------------------"
    echo "Done with configuration file setup."
    echo "Use the arp command to add static ARP entries"
    echo "------------------------------------------------"
    exit 1
}
#############################################################################

############################## NETMAP #######################################
step0_func_netmap()
{
    cd $CUR_DIR/samples
    for d in * ; do
	if [ -d $CUR_DIR/samples/$d/config ]; then
	    create_config $FL_NETMAP "samples/$d/config" $d
	fi
    done
    echo
    echo "------------------------------------------------"
    echo "Done with configuration file setup."
    echo "Use the arp command to add static ARP entries"
    echo "------------------------------------------------"
    exit 1
}
#############################################################################

############################# Main Script ###################################
export CUR_DIR=$PWD
kerver=$(uname -r)

if [ "$1" == "--compile-dpdk" ]; then
    vercomp $kerver "2.6.33"
    # if kernel version < 2.6.33
    if [ "$?" == "2" ]; then
	echo "[note] current kerner version ("$kerver") does not support DPDK"
	exit 1
    fi
    # Build and install DPDK library
    export DPDK_DIR="drivers/dpdk"
    export RTE_SDK="drivers/dpdk-16.11"
    export RTE_TARGET="x86_64-native-linuxapp-gcc"
    export DESTDIR="."
    echo
    echo "Selected DPDK library to be used for MOS"
    echo "----------------------------------------------------------"
    echo " RTE_SDK exported as $RTE_SDK"
    echo " RTE_TARGET exported as $RTE_TARGET"
    echo "----------------------------------------------------------"
    echo
    echo -n "Press enter to continue ..."; read    
    cd $RTE_SDK
    make install T=$RTE_TARGET
    echo
    echo -n "Done with DPDK setup. Press enter to start MOS setup ..."; read
    # Build and compile MOS library
    setup_mos_library $FL_DPDK
    
elif [ "$1" == "--compile-pcap" ]; then
    setup_mos_library $FL_PCAP

elif [ "$1" == "--compile-netmap" ]; then
    setup_mos_library $FL_NETMAP

elif [ "$1" == "--run-dpdk" ]; then
    export DPDK_DIR="drivers/dpdk"
    export RTE_SDK="drivers/dpdk-16.11"
    export RTE_TARGET="x86_64-native-linuxapp-gcc"
    while [ 1 ]; do
	clear
	echo
	echo "----------------------------------------------------------"
	echo " Full setup (from start)"
	echo "----------------------------------------------------------"
	echo "[0] Full setup for running mOS with DPDK"
	echo
	echo "----------------------------------------------------------"
	echo " Step-by-step setup for running mOS with DPDK"
	echo "----------------------------------------------------------"
	echo "[1] Setup hugepage mappings"
	echo "[2] Load and bind Ethernet devices to IGB_UIO module"
	echo "[3] Bring the interfaces up (DPDK devices)"
	echo "[4] Create new MOS configuration files for sample apps"
	echo "[5] Unbind dpdk-registered NICs"
	echo
	echo "[6] Exit script"
	echo	
	echo -n "Option: "
	read entry
	if [ "$entry" ]; then
	    step"$entry"_func_dpdk
	    echo
	    echo -n "Press enter to continue ..."; read
	fi
    done

elif [ "$1" == "--run-pcap" ]; then
    while [ 1 ]; do
	clear
	echo
	echo "----------------------------------------------------------"
	echo " Full setup (from start)"
	echo "----------------------------------------------------------"
	step0_func_pcap
	echo
	echo -n "Press enter to continue ..."; read
    done

elif [ "$1" == "--run-netmap" ]; then
    while [ 1 ]; do
	clear
	echo
	echo "----------------------------------------------------------"
	echo " Full setup (from start)"
	echo "----------------------------------------------------------"
	step0_func_netmap
	echo
	echo -n "Press enter to continue ..."; read
    done

elif [ "$1" == "--cleanup" ]; then
    find | grep mos.conf | xargs rm -f
    find | grep log__* | xargs rm -f

    for d in $CUR_DIR/samples/* ; do
	cd $d
	make clean > /dev/null 2> /dev/null
	cd ..
    done
    cd ..
    cd core/src
    make clean > /dev/null 2> /dev/null
    cd bpf
    rm -rf *.o
    cd ..
    rm -rf .*.d
    rm Makefile
    cd ../..
    find ./samples/ -name 'Makefile' | xargs rm -f
    rm -f scripts/config.log scripts/config.status
else
    echo "[error] please specify one of the following options"
    echo "--compile-dpdk   : compile and build mOS library with dpdk"
    echo "--compile-pcap   : compile and build mOS library with pcap"
    echo "--compile-netmap : compile and build mOS library with netmap"
    echo "--run-dpdk       : setup environments and configurations for running applications with dpdk"
    echo "--run-pcap       : setup environments and configurations for running applications with pcap"
    echo "--run-netmap     : setup environments and configurations for running applications with netmap"
    echo "--cleanup        : delete all binaries, config and Makefiles"
fi

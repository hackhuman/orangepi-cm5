#!/bin/bash

# arguments: $RELEASE $LINUXFAMILY $BOARD $BUILD_DESKTOP
#
# This is the image customization script

# NOTE: It is copied to /tmp directory inside the image
# and executed there inside chroot environment
# so don't reference any files that are not already installed

# NOTE: If you want to transfer files between chroot and host
# userpatches/overlay directory on host is bind-mounted to /tmp/overlay in chroot
# The sd card's root path is accessible via $SDCARD variable.

RELEASE=$1
LINUXFAMILY=$2
BOARD=$3
BUILD_DESKTOP=$4

Main() {
	case $RELEASE in
		xenial)
			# your code here
			;;
		stretch)
			# your code here
			# InstallOpenMediaVault # uncomment to get an OMV 4 image
			;;
		buster)
			# your code here
			;;
		bullseye)
			# your code here
			;;
		bionic)
			# your code here
			;;
		focal)
			# your code here
			;;
	esac
	SetStaticEthernetIP
	SetEnvironmentVars
	InstallQt514
	RemoveSystemQt515
	InstallIicTools
	TweakSshConfig
} # Main

SetEnvironmentVars() {
	# 在镜像内写入系统级环境变量
	#
	# 位置 1: /etc/environment
	#   - 所有经 PAM 登录的 shell 都会读到(本地终端、ssh、GUI)
	#   - 格式: VAR="value"(一行一个),引号内不要展开 $HOME 之类
	#   - 注意: systemd 服务默认不读它,若要给守护进程用请看 profile.d 或 systemd
	cat > /etc/environment <<- EOF
	LANG=en_US.UTF-8
	MY_APP_HOME=/opt/myapp
	EOF

	# 位置 2: /etc/profile.d/*.sh(可加 export、可做判断)
	#   登录 shell 会 source 这里的所有 .sh,支持 bash 语法
	cat > /etc/profile.d/myenv.sh <<- 'EOF'
	# 自定义环境变量(登录 shell 生效)
	export MY_APP_DEBUG=1
	export PATH="/opt/myapp/bin:$PATH"
	export LD_LIBRARY_PATH=/usr/lib/qt514:/usr/lib:$LD_LIBRARY_PATH
	EOF
	chmod 644 /etc/profile.d/myenv.sh
} # SetEnvironmentVars

InstallQt514() {
	# 原生(BSP 包)注入 —— 见 external/packages/bsp/common/:
	#   usr/lib/qt514/...        -> /usr/lib/qt514/...   (库文件,你自备)
	#   etc/ld.so.conf.d/qt514.conf                       (指向上面目录)
	# BSP deb 先于本脚本安装到 rootfs(distributions.sh),这里只让
	# ld.so 重载配置,使所有进程(含 systemd 服务)都能找到 Qt 库。
	ldconfig || true
} # InstallQt514

RemoveSystemQt515() {
	# 构建期一次性移除系统 apt 的 Qt 5.15 库,避免程序混入 5.15 而报
	# "Cannot mix incompatible Qt library"。本机程序必须用自带 5.14,
	# 系统 5.15 会导致 5.14.1 主库 + 5.15.3 插件同时进进程 -> 拒绝启动。
	#
	# NOTE: 必须删整条链的 3 个节点,且必须删真身(.5.15.3),否则 ldconfig
	#       会依据残留真身把两条软链接(.so.5 / .so.5.15)重新建回来:
	#       .so.5 -> .so.5.15 -> .so.5.15.3(真身)
	# 只动 Core/Network 两条链;不触碰 libqt5gui5 等其它包的文件。
	# 代价:镜像内依赖系统 5.15 的包(桌面/webengine 等)丢失该库,勿再 apt。
	local d=/usr/lib/aarch64-linux-gnu
	for base in libQt5Core libQt5Network; do
		rm -f "${d}/${base}.so.5" "${d}/${base}.so.5.15"    # 软链接
		rm -f "${d}/${base}.so.5.15.3"                       # 真身,ldconfig 无源可建
	done
	ldconfig || true
} # RemoveSystemQt515

InstallIicTools() {
	# 内置相机 MCU 工具树到 /root/iic,并补齐运行时权限。
	#
	# 文件本体经 BSP 包内置:放在 external/packages/bsp/common/root/iic/
	# (makeboarddeb.sh 里 rsync common/* 到 deb 根 -> 装到镜像 /)。
	# BSP 打包会跑 dh_fixperms(chmod 'go=rX,u+rw,a-s'),不会给已有文件加 +x;
	# 且 Samba 落盘时 +x 位也常丢,所以这里显式补齐 +x(脚本/二进制)。
	local iic=/root/iic

	if [ ! -d "$iic" ]; then
		echo "[iic] not present at ${iic}, skip (did you stage external/packages/bsp/common/root/iic/?)"
		return 0
	fi

	find "$iic" -type f \( \
		-name '*.sh' \
		-o -name 'i2c_read' -o -name 'i2c_write' \
		-o -name 'i2c_4read' -o -name 'i2c_4write' \
		-o -name 'lut_rw' -o -name 'veye_i2c_upgrade' \
	\) -exec chmod +x {} +

	find "$iic" -type d -exec chmod 0755 {} +

	echo "[iic] tool tree present & perms set: ${iic}"
} # InstallIicTools

TweakSshConfig() {
	# 兼容旧版 SSH 客户端:把镜像里较新的 sshd 已从默认集合
	# 移除的算法加回去。用 '+xxx' 追加到默认集合,不影响其余默认项。
	# 注意:这里只能追加,不能整文件覆盖(会丢掉发行版默认配置)。
	local sshd=/etc/ssh/sshd_config
	[ -f "$sshd" ] || return 0
	echo "KexAlgorithms +diffie-hellman-group1-sha1" >> "$sshd"
	echo "HostKeyAlgorithms +ssh-rsa" >> "$sshd"
} # TweakSshConfig

SetStaticEthernetIP() {
	# 目标系统的有线网口名：RK3588 通常为 end0 或 eth0，可用
	# LANG=C nmcli -t -f GENERAL.DEVICE dev status 在目标机上确认后固定
	local iface="enP3p49s0"
	local ip="192.168.2.3"
	local mask=24
	local gw="192.168.2.1"
	local dns1=("192.168.2.1" "8.8.8.8")

	cat > /etc/NetworkManager/system-connections/"${iface}".nmconnection <<- EOF
	[connection]
	id=${iface}
	uuid=$(cat /proc/sys/kernel/random/uuid)
	type=ethernet
	interface-name=${iface}
	autoconnect=true

	[ipv4]
	method=manual
	address1=${ip}/${mask},${gw}
	dns=${dns1[@]}
	EOF
	chmod 600 /etc/NetworkManager/system-connections/"${iface}".nmconnection
}

InstallOpenMediaVault() {
	# use this routine to create a Debian based fully functional OpenMediaVault
	# image (OMV 3 on Jessie, OMV 4 with Stretch). Use of mainline kernel highly
	# recommended!
	#
	# Please note that this variant changes Orange Pi default security 
	# policies since you end up with root password 'openmediavault' which
	# you have to change yourself later. SSH login as root has to be enabled
	# through OMV web UI first
	#
	# This routine is based on idea/code courtesy Benny Stark. For fixes,
	# discussion and feature requests please refer to
	# https://forum.armbian.com/index.php?/topic/2644-openmediavault-3x-customize-imagesh/

	echo root:openmediavault | chpasswd
	rm /root/.not_logged_in_yet
	. /etc/default/cpufrequtils
	export LANG=C LC_ALL="en_US.UTF-8"
	export DEBIAN_FRONTEND=noninteractive
	export APT_LISTCHANGES_FRONTEND=none

	case ${RELEASE} in
		jessie)
			OMV_Name="erasmus"
			OMV_EXTRAS_URL="https://github.com/OpenMediaVault-Plugin-Developers/packages/raw/master/openmediavault-omvextrasorg_latest_all3.deb"
			;;
		stretch)
			OMV_Name="arrakis"
			OMV_EXTRAS_URL="https://github.com/OpenMediaVault-Plugin-Developers/packages/raw/master/openmediavault-omvextrasorg_latest_all4.deb"
			;;
	esac

	# Add OMV source.list and Update System
	cat > /etc/apt/sources.list.d/openmediavault.list <<- EOF
	deb https://openmediavault.github.io/packages/ ${OMV_Name} main
	## Uncomment the following line to add software from the proposed repository.
	deb https://openmediavault.github.io/packages/ ${OMV_Name}-proposed main
	
	## This software is not part of OpenMediaVault, but is offered by third-party
	## developers as a service to OpenMediaVault users.
	# deb https://openmediavault.github.io/packages/ ${OMV_Name} partner
	EOF

	# Add OMV and OMV Plugin developer keys, add Cloudshell 2 repo for XU4
	if [ "${BOARD}" = "odroidxu4" ]; then
		add-apt-repository -y ppa:kyle1117/ppa
		sed -i 's/jessie/xenial/' /etc/apt/sources.list.d/kyle1117-ppa-jessie.list
	fi
	mount --bind /dev/null /proc/mdstat
	apt-get update
	apt-get --yes --force-yes --allow-unauthenticated install openmediavault-keyring
	apt-key adv --keyserver hkp://keyserver.ubuntu.com:80 --recv-keys 7AA630A1EDEE7D73
	apt-get update

	# install debconf-utils, postfix and OMV
	HOSTNAME="${BOARD}"
	debconf-set-selections <<< "postfix postfix/mailname string ${HOSTNAME}"
	debconf-set-selections <<< "postfix postfix/main_mailer_type string 'No configuration'"
	apt-get --yes --force-yes --allow-unauthenticated  --fix-missing --no-install-recommends \
		-o Dpkg::Options::="--force-confdef" -o Dpkg::Options::="--force-confold" install \
		debconf-utils postfix
	# move newaliases temporarely out of the way (see Ubuntu bug 1531299)
	cp -p /usr/bin/newaliases /usr/bin/newaliases.bak && ln -sf /bin/true /usr/bin/newaliases
	sed -i -e "s/^::1         localhost.*/::1         ${HOSTNAME} localhost ip6-localhost ip6-loopback/" \
		-e "s/^127.0.0.1   localhost.*/127.0.0.1   ${HOSTNAME} localhost/" /etc/hosts
	sed -i -e "s/^mydestination =.*/mydestination = ${HOSTNAME}, localhost.localdomain, localhost/" \
		-e "s/^myhostname =.*/myhostname = ${HOSTNAME}/" /etc/postfix/main.cf
	apt-get --yes --force-yes --allow-unauthenticated  --fix-missing --no-install-recommends \
		-o Dpkg::Options::="--force-confdef" -o Dpkg::Options::="--force-confold" install \
		openmediavault

	# install OMV extras, enable folder2ram and tweak some settings
	FILE=$(mktemp)
	wget "$OMV_EXTRAS_URL" -qO $FILE && dpkg -i $FILE
	
	/usr/sbin/omv-update
	# Install flashmemory plugin and netatalk by default, use nice logo for the latter,
	# tweak some OMV settings
	. /usr/share/openmediavault/scripts/helper-functions
	apt-get -y -q install openmediavault-netatalk openmediavault-flashmemory
	AFP_Options="mimic model = Macmini"
	SMB_Options="min receivefile size = 16384\nwrite cache size = 524288\ngetwd cache = yes\nsocket options = TCP_NODELAY IPTOS_LOWDELAY"
	xmlstarlet ed -L -u "/config/services/afp/extraoptions" -v "$(echo -e "${AFP_Options}")" /etc/openmediavault/config.xml
	xmlstarlet ed -L -u "/config/services/smb/extraoptions" -v "$(echo -e "${SMB_Options}")" /etc/openmediavault/config.xml
	xmlstarlet ed -L -u "/config/services/flashmemory/enable" -v "1" /etc/openmediavault/config.xml
	xmlstarlet ed -L -u "/config/services/ssh/enable" -v "1" /etc/openmediavault/config.xml
	xmlstarlet ed -L -u "/config/services/ssh/permitrootlogin" -v "0" /etc/openmediavault/config.xml
	xmlstarlet ed -L -u "/config/system/time/ntp/enable" -v "1" /etc/openmediavault/config.xml
	xmlstarlet ed -L -u "/config/system/time/timezone" -v "UTC" /etc/openmediavault/config.xml
	xmlstarlet ed -L -u "/config/system/network/dns/hostname" -v "${HOSTNAME}" /etc/openmediavault/config.xml
	xmlstarlet ed -L -u "/config/system/monitoring/perfstats/enable" -v "0" /etc/openmediavault/config.xml
	echo -e "OMV_CPUFREQUTILS_GOVERNOR=${GOVERNOR}" >>/etc/default/openmediavault
	echo -e "OMV_CPUFREQUTILS_MINSPEED=${MIN_SPEED}" >>/etc/default/openmediavault
	echo -e "OMV_CPUFREQUTILS_MAXSPEED=${MAX_SPEED}" >>/etc/default/openmediavault
	for i in netatalk samba flashmemory ssh ntp timezone interfaces cpufrequtils monit collectd rrdcached ; do
		/usr/sbin/omv-mkconf $i
	done
	/sbin/folder2ram -enablesystemd || true
	sed -i 's|-j /var/lib/rrdcached/journal/ ||' /etc/init.d/rrdcached

	# Fix multiple sources entry on ARM with OMV4
	sed -i '/stretch-backports/d' /etc/apt/sources.list

	# rootfs resize to 7.3G max and adding omv-initsystem to firstrun -- q&d but shouldn't matter
	echo 15500000s >/root/.rootfs_resize
	sed -i '/systemctl\ disable\ orangepi-firstrun/i \
	mv /usr/bin/newaliases.bak /usr/bin/newaliases \
	export DEBIAN_FRONTEND=noninteractive \
	sleep 3 \
	apt-get install -f -qq python-pip python-setuptools || exit 0 \
	pip install -U tzupdate \
	tzupdate \
	read TZ </etc/timezone \
	/usr/sbin/omv-initsystem \
	xmlstarlet ed -L -u "/config/system/time/timezone" -v "${TZ}" /etc/openmediavault/config.xml \
	/usr/sbin/omv-mkconf timezone \
	lsusb | egrep -q "0b95:1790|0b95:178a|0df6:0072" || sed -i "/ax88179_178a/d" /etc/modules' /usr/lib/orangepi/orangepi-firstrun
	sed -i '/systemctl\ disable\ orangepi-firstrun/a \
	sleep 30 && sync && reboot' /usr/lib/orangepi/orangepi-firstrun

	# add USB3 Gigabit Ethernet support
	echo -e "r8152\nax88179_178a" >>/etc/modules

	case ${BOARD} in
		odroidxu4)
			HMP_Fix='; taskset -c -p 4-7 $i '
			# Cloudshell stuff (fan, lcd, missing serials on 1st CS2 batch)
			echo "H4sIAKdXHVkCA7WQXWuDMBiFr+eveOe6FcbSrEIH3WihWx0rtVbUFQqCqAkYGhJn
			tF1x/vep+7oebDfh5DmHwJOzUxwzgeNIpRp9zWRegDPznya4VDlWTXXbpS58XJtD
			i7ICmFBFxDmgI6AXSLgsiUop54gnBC40rkoVA9rDG0SHHaBHPQx16GN3Zs/XqxBD
			leVMFNAz6n6zSWlEAIlhEw8p4xTyFtwBkdoJTVIJ+sz3Xa9iZEMFkXk9mQT6cGSQ
			QL+Cr8rJJSmTouuuRzfDtluarm1aLVHksgWmvanm5sbfOmY3JEztWu5tV9bCXn4S
			HB8RIzjoUbGvFvPw/tmr0UMr6bWSBupVrulY2xp9T1bruWnVga7DdAqYFgkuCd3j
			vORUDQgej9HPJxmDDv+3WxblBSuYFH8oiNpHz8XvPIkU9B3JVCJ/awIAAA==" \
			| tr -d '[:blank:]' | base64 --decode | gunzip -c >/usr/local/sbin/cloudshell2-support.sh
			chmod 755 /usr/local/sbin/cloudshell2-support.sh
			apt install -y i2c-tools odroid-cloudshell cloudshell2-fan
			sed -i '/systemctl\ disable\ orangepi-firstrun/i \
			lsusb | grep -q -i "05e3:0735" && sed -i "/exit\ 0/i echo 20 > /sys/class/block/sda/queue/max_sectors_kb" /etc/rc.local \
			/usr/sbin/i2cdetect -y 1 | grep -q "60: 60" && /usr/local/sbin/cloudshell2-support.sh' /usr/lib/orangepi/orangepi-firstrun
			;;
		bananapim3|nanopifire3|nanopct3plus|nanopim3)
			HMP_Fix='; taskset -c -p 4-7 $i '
			;;
		edge*|ficus|firefly-rk3399|nanopct4|nanopim4|nanopineo4|renegade-elite|roc-rk3399-pc|rockpro64)
			HMP_Fix='; taskset -c -p 4-5 $i '
			;;
	esac
	echo "* * * * * root for i in \`pgrep \"ftpd|nfsiod|smbd|afpd|cnid\"\` ; do ionice -c1 -p \$i ${HMP_Fix}; done >/dev/null 2>&1" \
		>/etc/cron.d/make_nas_processes_faster
	chmod 600 /etc/cron.d/make_nas_processes_faster

	# add SATA port multiplier hint if appropriate
	[ "${LINUXFAMILY}" = "sunxi" ] && \
		echo -e "#\n# If you want to use a SATA PM add \"ahci_sunxi.enable_pmp=1\" to bootargs above" \
		>>/boot/boot.cmd

	# Filter out some log messages
	echo ':msg, contains, "do ionice -c1" ~' >/etc/rsyslog.d/omv-orangepi.conf
	echo ':msg, contains, "action " ~' >>/etc/rsyslog.d/omv-orangepi.conf
	echo ':msg, contains, "netsnmp_assert" ~' >>/etc/rsyslog.d/omv-orangepi.conf
	echo ':msg, contains, "Failed to initiate sched scan" ~' >>/etc/rsyslog.d/omv-orangepi.conf

	# Fix little python bug upstream Debian 9 obviously ignores
	if [ -f /usr/lib/python3.5/weakref.py ]; then
		wget -O /usr/lib/python3.5/weakref.py \
		https://raw.githubusercontent.com/python/cpython/9cd7e17640a49635d1c1f8c2989578a8fc2c1de6/Lib/weakref.py
	fi

	# clean up and force password change on first boot
	umount /proc/mdstat
	chage -d 0 root
} # InstallOpenMediaVault

UnattendedStorageBenchmark() {
	# Function to create Orange Pi images ready for unattended storage performance testing.
	# Useful to use the same OS image with a bunch of different SD cards or eMMC modules
	# to test for performance differences without wasting too much time.

	rm /root/.not_logged_in_yet

	apt-get -qq install time

	wget -qO /usr/local/bin/sd-card-bench.sh https://raw.githubusercontent.com/ThomasKaiser/sbc-bench/master/sd-card-bench.sh
	chmod 755 /usr/local/bin/sd-card-bench.sh

	sed -i '/^exit\ 0$/i \
	/usr/local/bin/sd-card-bench.sh &' /etc/rc.local
} # UnattendedStorageBenchmark

InstallAdvancedDesktop()
{
	apt-get install -yy transmission libreoffice libreoffice-style-tango meld remmina thunderbird kazam avahi-daemon
	[[ -f /usr/share/doc/avahi-daemon/examples/sftp-ssh.service ]] && cp /usr/share/doc/avahi-daemon/examples/sftp-ssh.service /etc/avahi/services/
	[[ -f /usr/share/doc/avahi-daemon/examples/ssh.service ]] && cp /usr/share/doc/avahi-daemon/examples/ssh.service /etc/avahi/services/
	apt clean
} # InstallAdvancedDesktop

Main "$@"

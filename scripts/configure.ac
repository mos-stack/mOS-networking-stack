AC_INIT([mos_release], version-0.3, [ajamshed@ndsl.kaist.edu])

# Check for cc
AC_PROG_CC
#AC_PROG_INSTALL
AC_PROG_RANLIB
#LT_INIT
AM_PROG_CC_C_O
#AC_PROG_LIBTOOL
AC_LANG(C)
AC_DISABLE_OPTION_CHECKING

# Check for scheduling
AC_CHECK_HEADER(linux/sched.h,,,) 
# Check for pthreads
AC_CHECK_HEADER(pthread.h,,,)
# Check for numa 
AC_CHECK_HEADER(numa.h,,,)
AC_CHECK_HEADERS([fcntl.h inttypes.h limits.h stddef.h stdlib.h string.h strings.h sys/param.h sys/time.h unistd.h])

# Check for typedefs, structures, and compiler characteristics
AC_C_INLINE
AC_TYPE_OFF_T
AC_TYPE_SIZE_T
AC_TYPE_SSIZE_T
AC_TYPE_UINT16_T
AC_TYPE_UINT32_T
AC_TYPE_UINT8_T

# Checks for library functions.
AC_FUNC_LSTAT_FOLLOWS_SLASHED_SYMLINK
AC_FUNC_MALLOC
AC_CHECK_FUNCS([ftruncate gettimeofday memset strcasecmp strchr strpbrk strrchr strstr])

# Reset DPDK to 0
AC_SUBST(DPDK, 0)
# Reset PSIO to 0
AC_SUBST(PSIO, 0)
# Reset Netmap to 0
AC_SUBST(NETMAP, 0)
# Reset Pcap to 0
AC_SUBST(PCAP, 0)

# Enable dpdk
AC_ARG_ENABLE([dpdk],
	  AS_HELP_STRING([--enable-dpdk], [Enable dpdk-16.04]))

AS_IF([test "x$enable_dpdk" = "xyes"], [
	 dnl Do the stuff needed for enabling the feature
      	 AC_SUBST(DPDK, 1)
])

# Enable netmap
AC_ARG_ENABLE([netmap],
	  AS_HELP_STRING([--enable-netmap], [Enable netmap]))

AS_IF([test "x$enable_netmap" = "xyes"], [
	 dnl Do the stuff needed for enabling the feature
      	 AC_SUBST(NETMAP, 1)
])

# Enable psio
AC_ARG_ENABLE([psio],
	  AS_HELP_STRING([--enable-psio], [Enable packetshader I/O]))

AS_IF([test "x$enable_psio" = "xyes"], [
	 dnl Do the stuff needed for enabling the feature
      	 AC_SUBST(PSIO, 1)
])

if test "$DPDK" == "0" && test "$PSIO" == "0" && test "$NETMAP" == "0"
then
        AC_MSG_WARN([Packet I/O library is missing. Please set either dpdk, netmap or psio as your I/O lib.I am choosing pcap as your I/O lib!])
	AC_SUBST(PCAP, 1)
fi

AC_OUTPUT(../core/src/Makefile)
AC_OUTPUT(../samples/epserver/Makefile)
AC_OUTPUT(../samples/epwget/Makefile)
AC_OUTPUT(../samples/midstat/Makefile)
AC_OUTPUT(../samples/simple_firewall/Makefile)
AC_OUTPUT(../samples/nat/Makefile)


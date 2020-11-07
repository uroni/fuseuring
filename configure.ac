#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ(2.61)
AC_INIT([fuseuring], [0.1.0.0], [martin@urbackup.org])
AC_CONFIG_SRCDIR([main.cpp])
AC_CONFIG_HEADER([config.h])
AC_CONFIG_MACRO_DIR([m4])
AC_CANONICAL_SYSTEM
AM_INIT_AUTOMAKE([subdir-objects tar-ustar])

# Checks for programs.
AC_PROG_CXX
AC_PROG_CC
AM_PROG_CC_C_O

AX_PTHREAD
if !($HAVE_PTHREAD)
then
	echo "Sorry, your system needs the pthread library."
	echo "Either install it or give up."
	exit 1
fi

AC_LANG([C++])

# Checks for libraries.

# Checks for header files.
AC_CHECK_HEADERS([pthread.h])

# Checks for library functions.

AC_SEARCH_LIBS([clock_gettime], [rt posix4],
	[test "$ac_cv_search_clock_gettime" = "none required" || LIBS="$LIBS $ac_cv_search_clock_gettime"],
	[AC_MSG_FAILURE([No library for clock_gettime found])] )
	
AC_CONFIG_FILES([Makefile])
AC_OUTPUT

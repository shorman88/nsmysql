#
# $Id: Makefile,v 1.3 2001/10/19 03:37:26 dossy Exp $
#

NSHOME    =  ../aolserver

#
# Module name
#
MOD       =  nsmysql.so

#
# Objects to build.
#
OBJS      =  mysql.o

#
# Set this to be the installation prefix of your MySQL installation.
#
MYSQL_PREFIX = /usr/local

MYSQL_LIBDIR = $(MYSQL_PREFIX)/lib/mysql
MYSQL_INCDIR = $(MYSQL_PREFIX)/include/mysql

#
# Header files in THIS directory
#
HDRS     =

#
# Extra libraries
#
MODLIBS  =  -R$(MYSQL_LIBDIR) -L$(MYSQL_LIBDIR) -lmysqlclient_r

# Uncomment the next line if you have problems relating to
# "compress" ...

# MODLIBS  +=  -lz

#
# Compiler flags
#
CFLAGS   = -I$(MYSQL_INCDIR)


include  $(NSHOME)/include/Makefile.module


# Override linker to use ld(1), gcc doesn't understand -R ...
LDSO     = ld -shared


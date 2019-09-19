# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
# 
#   http://www.apache.org/licenses/LICENSE-2.0
# 
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.
DATA_built = create_udv.sql
MODULE_big = vexecutor
OBJS    = vtype.o \

EXTRA_CLEAN = create_udv.sql
PG_CXXFLAGS = -Wall  -O3 -g  -msse -msse2 -msse3 -msse4 -march=native -fopt-info-vec
PG_LIBS = $(libpq_pgport) 

PG_CONFIG = pg_config

ifdef USE_PGXS
PGXS := $(shell pg_config --pgxs)
include $(PGXS)
else
subdir = contrib/vexecutor
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

SHLIB_LINK += $(filter -lz -lsnappy, $(LIBS))

$(srcdir)/create_udv.sql : create_udv.sql.in
	cat $< > $@

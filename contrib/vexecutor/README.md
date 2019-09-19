Server: 
gcloud beta compute --project "gpdb-mpp" ssh --zone "asia-northeast1-b" huanzhang@"hubert-gp-centos"


gpdb7 repo with batch optimiztion: /home/huanzhang/workspace/gpdb7
branch: batch
source /usr/local/greenplum-db-devel-7/greenplum_path.sh
MASTER_DATA_DIRECTORY=/home/huanzhang/workspace/gpdb7/gpAux/gpdemo/datadirs/qddir/demoDataDir-1


BUILD gpdb:

CFLAGS="-O3 -g -march=native -fopt-info-vec" ./configure --prefix=/usr/local/greenplum-db-devel-7/ --enable-depend --disable-orca  -q --enable-debug --with-python --disable-gpcloud

Build contrib/vexecutor
USE_PGXS=1 make install

and run psql -f create_udv.sql
